/**
 * xfyun_tts.c - 讯飞在线语音合成 (TTS) C 接入示例
 *
 * 功能: 将文本通过讯飞 WebSocket TTS API 合成为 PCM 音频文件
 * API:  wss://tts-api.xfyun.cn/v2/tts
 *
 * 鉴权: URL 携带 HMAC-SHA256 签名 (OpenSSL 实现，无需额外依赖)
 *
 * 运行前设置环境变量:
 *   export XFYUN_APPID=your_appid
 *   export XFYUN_APIKEY=your_apikey
 *   export XFYUN_APISECRET=your_apisecret
 *
 * 编译:
 *   cmake --build build
 *
 * 运行:
 *   ./build/app/xfyun_tts "待合成的文本" [output.pcm]
 *
 * 播放 (需要 sox 或 aplay):
 *   sox -t raw -r 16000 -e signed -b 16 -c 1 demo.pcm demo.wav
 *   aplay -r 16000 -f S16_LE -c 1 demo.pcm
 */

#include "ws_pool.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ==================== API 常量 ==================== */

/* 注意: TTS 签名中的 host 与实际连接 host 不同，这是讯飞 API 的固定格式 */
#define TTS_CONN_HOST "tts-api.xfyun.cn"  /* 实际 WebSocket 连接地址 */
#define TTS_SIG_HOST  "ws-api.xfyun.cn"   /* 签名原文中的 host 字段   */
#define TTS_PORT      443
#define TTS_PATH      "/v2/tts"

/* 音频参数 */
#define TTS_AUE "raw"                   /* raw PCM, 16kHz 16-bit mono */
#define TTS_AUF "audio/L16;rate=16000"
#define TTS_VCN "x4_yezi"              /* 发音人，可替换: xiaoyan, aisjiuxu... */
#define TTS_TTE "utf8"

#define STATUS_LAST_FRAME 2
#define TTS_TIMEOUT_SEC   30

/* ==================== TTS 会话状态 ==================== */

typedef struct {
    FILE            *pcm_file;
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    volatile int     done;
    int              api_code;
    char             sid[64];
    long             total_bytes;
} tts_ctx_t;

/* ==================== Base64 (OpenSSL EVP) ==================== */

/* 编码: 返回写入 out 的字节数（不含 '\0'），失败返回 0 */
static size_t b64_encode(const unsigned char *in, size_t inlen,
                         char *out, size_t outlen)
{
    int n = EVP_EncodeBlock((unsigned char *)out, in, (int)inlen);
    if (n < 0 || (size_t)(n + 1) > outlen) return 0;
    out[n] = '\0';
    return (size_t)n;
}

/* 解码: 返回写入 out 的字节数，失败返回 0 */
static size_t b64_decode(const char *in, size_t inlen,
                         unsigned char *out, size_t outlen)
{
    if (inlen == 0) return 0;

    /* EVP_DecodeBlock 返回值含尾部 padding 带来的 0 字节 */
    int n = EVP_DecodeBlock(out, (const unsigned char *)in, (int)inlen);
    if (n < 0) return 0;

    size_t pad = 0;
    if (inlen >= 1 && in[inlen - 1] == '=') pad++;
    if (inlen >= 2 && in[inlen - 2] == '=') pad++;

    size_t decoded = (size_t)n > pad ? (size_t)n - pad : 0;
    return decoded < outlen ? decoded : outlen;
}

/* ==================== HMAC-SHA256 ==================== */

/* 计算 HMAC-SHA256 并 base64 编码，结果写入 out (至少 48 字节) */
static int hmac_sha256_b64(const char *key,  size_t keylen,
                           const char *data, size_t datalen,
                           char *out, size_t outlen)
{
    unsigned char digest[32];
    unsigned int  dlen = 0;

    if (!HMAC(EVP_sha256(),
              key, (int)keylen,
              (const unsigned char *)data, (int)datalen,
              digest, &dlen)) {
        return -1;
    }
    return b64_encode(digest, dlen, out, outlen) > 0 ? 0 : -1;
}

/* ==================== URL 工具 ==================== */

static void rfc1123_now(char *buf, size_t buflen)
{
    time_t t = time(NULL);
    struct tm tm_gmt;
    gmtime_r(&t, &tm_gmt);
    strftime(buf, buflen, "%a, %d %b %Y %H:%M:%S GMT", &tm_gmt);
}

/* URL percent-encode (RFC 3986 严格模式：仅保留 unreserved chars) */
static void url_encode(const char *in, char *out, size_t outlen)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 4 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 0x0f];
        }
    }
    out[j] = '\0';
}

/* ==================== 鉴权路径构造 ==================== */

/*
 * build_tts_path - 生成带鉴权参数的 WebSocket 路径
 *
 * 鉴权流程（与官方 Python/Go SDK 等效）:
 *   1. RFC1123 时间戳
 *   2. 签名原文: "host: <sig_host>\ndate: <date>\nGET <path> HTTP/1.1"
 *   3. HMAC-SHA256(api_secret, 签名原文) → base64
 *   4. authorization 原文 → base64
 *   5. 拼接为 URL 参数（各参数值需 percent-encode）
 */
static int build_tts_path(const char *api_key, const char *api_secret,
                           char *out, size_t outlen)
{
    char date[64];
    rfc1123_now(date, sizeof(date));

    /* 签名原文 */
    char sig_origin[512];
    snprintf(sig_origin, sizeof(sig_origin),
             "host: " TTS_SIG_HOST "\ndate: %s\nGET " TTS_PATH " HTTP/1.1",
             date);

    /* HMAC-SHA256 → base64 */
    char sig_b64[64];
    if (hmac_sha256_b64(api_secret, strlen(api_secret),
                        sig_origin, strlen(sig_origin),
                        sig_b64, sizeof(sig_b64)) != 0) {
        fprintf(stderr, "[TTS] HMAC-SHA256 failed\n");
        return -1;
    }

    /* authorization 原文 → base64 */
    char auth_origin[384];
    snprintf(auth_origin, sizeof(auth_origin),
             "api_key=\"%s\", algorithm=\"hmac-sha256\", "
             "headers=\"host date request-line\", signature=\"%s\"",
             api_key, sig_b64);

    char auth_b64[512];
    b64_encode((const unsigned char *)auth_origin, strlen(auth_origin),
               auth_b64, sizeof(auth_b64));

    /* URL percent-encode 各参数值 */
    char auth_enc[768], date_enc[128], host_enc[64];
    url_encode(auth_b64,      auth_enc, sizeof(auth_enc));
    url_encode(date,          date_enc, sizeof(date_enc));
    url_encode(TTS_SIG_HOST,  host_enc, sizeof(host_enc));

    int n = snprintf(out, outlen,
                     TTS_PATH "?authorization=%s&date=%s&host=%s",
                     auth_enc, date_enc, host_enc);
    return (n > 0 && (size_t)n < outlen) ? 0 : -1;
}

/* ==================== 请求 JSON ==================== */

static int build_request_json(const char *appid, const char *text,
                               char *out, size_t outlen)
{
    /* 文本 → base64 */
    char text_b64[4096];
    if (b64_encode((const unsigned char *)text, strlen(text),
                   text_b64, sizeof(text_b64)) == 0) {
        fprintf(stderr, "[TTS] text too long for base64 buffer\n");
        return -1;
    }

    int n = snprintf(out, outlen,
        "{"
          "\"common\":{\"app_id\":\"%s\"},"
          "\"business\":{"
            "\"aue\":\"" TTS_AUE "\","
            "\"auf\":\"" TTS_AUF "\","
            "\"vcn\":\"" TTS_VCN "\","
            "\"tte\":\"" TTS_TTE "\""
          "},"
          "\"data\":{\"status\":%d,\"text\":\"%s\"}"
        "}",
        appid, STATUS_LAST_FRAME, text_b64);

    return (n > 0 && (size_t)n < outlen) ? 0 : -1;
}

/* ==================== 简易 JSON 字段提取 ==================== */

/* 提取 "key": number，返回 1 表示成功 */
static int json_get_int(const char *json, const char *key, int *val)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    return sscanf(p, "%d", val) == 1;
}

/* 提取 "key": "value"，复制到 out（最多 outlen-1 字节） */
static int json_get_str(const char *json, const char *key,
                         char *out, size_t outlen)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len >= outlen) len = outlen - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

/* ==================== 接收回调（在 service 线程中调用）==================== */

static void on_tts_message(const char *data, size_t len, int is_binary, void *user)
{
    tts_ctx_t *ctx = (tts_ctx_t *)user;
    (void)is_binary;

    /* 拷贝一份保证 '\0' 结尾 */
    char *json = malloc(len + 1);
    if (!json) return;
    memcpy(json, data, len);
    json[len] = '\0';

    /* 检查 API 返回码 */
    int code = -1;
    if (!json_get_int(json, "code", &code) || code != 0) {
        char msg[256] = "(unknown error)";
        json_get_str(json, "message", msg, sizeof(msg));
        json_get_str(json, "sid",     ctx->sid, sizeof(ctx->sid));
        fprintf(stderr, "[TTS] API error: code=%d sid=%s msg=%s\n",
                code, ctx->sid, msg);
        pthread_mutex_lock(&ctx->lock);
        ctx->api_code = code;
        ctx->done     = 1;
        pthread_cond_signal(&ctx->cond);
        pthread_mutex_unlock(&ctx->lock);
        free(json);
        return;
    }

    json_get_str(json, "sid", ctx->sid, sizeof(ctx->sid));

    /* 解码 data.audio（base64 → PCM）*/
    char *audio_b64 = malloc(len + 4);   /* audio 字段不会超过总长度 */
    if (audio_b64) {
        if (json_get_str(json, "audio", audio_b64, len + 4)) {
            size_t b64len = strlen(audio_b64);
            if (b64len > 0) {
                size_t pcm_max = b64len * 3 / 4 + 4;
                unsigned char *pcm = malloc(pcm_max);
                if (pcm) {
                    size_t pcm_len = b64_decode(audio_b64, b64len, pcm, pcm_max);
                    if (pcm_len > 0) {
                        fwrite(pcm, 1, pcm_len, ctx->pcm_file);
                        ctx->total_bytes += (long)pcm_len;
                    }
                    free(pcm);
                }
            }
        }
        free(audio_b64);
    }

    /* 检查 data.status */
    int status = 0;
    json_get_int(json, "status", &status);
    printf("[TTS] sid=%-32s  status=%d  total=%ld bytes\n",
           ctx->sid, status, ctx->total_bytes);

    if (status == STATUS_LAST_FRAME) {
        pthread_mutex_lock(&ctx->lock);
        ctx->done = 1;
        pthread_cond_signal(&ctx->cond);
        pthread_mutex_unlock(&ctx->lock);
    }

    free(json);
}

/* ==================== 信号处理 ==================== */

static volatile int g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ==================== 主函数 ==================== */

int main(int argc, char *argv[])
{
    const char *appid      = getenv("XFYUN_APPID");
    const char *api_key    = getenv("XFYUN_APIKEY");
    const char *api_secret = getenv("XFYUN_APISECRET");
    const char *text       = (argc > 1) ? argv[1] : "这是一个语音合成示例。";
    const char *out_file   = (argc > 2) ? argv[2] : "./demo.pcm";

    if (!appid || !api_key || !api_secret) {
        fprintf(stderr,
            "Error: missing credentials\n\n"
            "  export XFYUN_APPID=your_appid\n"
            "  export XFYUN_APIKEY=your_apikey\n"
            "  export XFYUN_APISECRET=your_apisecret\n\n"
            "Usage: %s [text] [output.pcm]\n", argv[0]);
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* 1. 构造鉴权 URL 路径 */
    char path[2048];
    if (build_tts_path(api_key, api_secret, path, sizeof(path)) != 0)
        return 1;

    printf("[TTS] text     : %s\n", text);
    printf("[TTS] output   : %s\n", out_file);
    printf("[TTS] endpoint : wss://" TTS_CONN_HOST ":%d" TTS_PATH "\n\n", TTS_PORT);

    /* 2. 初始化 TTS 上下文 */
    tts_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.lock, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    ctx.pcm_file = fopen(out_file, "wb");
    if (!ctx.pcm_file) {
        perror("fopen");
        return 1;
    }

    /* 3. 创建连接池和 TTS 客户端 */
    wsp_pool_t   *pool   = wsp_pool_create();
    wsp_client_t *client = wsp_client_create(TTS_CONN_HOST, TTS_PORT,
                                              NULL, on_tts_message, &ctx);
    if (!pool || !client) {
        fprintf(stderr, "[TTS] create pool/client failed\n");
        fclose(ctx.pcm_file);
        return 1;
    }

    wsp_set_ssl(client, 1, 1);              /* WSS, 跳过证书验证（测试用）*/
    wsp_set_path(client, path);
    wsp_set_reconnect(client, 2000, 2000);  /* TTS 为一次性连接，限制重连 */

    if (wsp_pool_add(pool, client) != LWS_OK) {
        fprintf(stderr, "[TTS] wsp_pool_add failed\n");
        wsp_client_destroy(client);
        wsp_pool_destroy(pool);
        fclose(ctx.pcm_file);
        return 1;
    }

    /* 4. 启动连接池（建立 WebSocket 连接）*/
    if (!wsp_pool_start(pool)) {
        fprintf(stderr, "[TTS] wsp_pool_start failed\n");
        wsp_pool_destroy(pool);
        fclose(ctx.pcm_file);
        return 1;
    }

    /* 5. 等待连接建立后发送合成请求 */
    printf("[TTS] Waiting for connection...\n");
    sleep(2);

    if (g_stop) goto cleanup;

    char req_json[8192];
    if (build_request_json(appid, text, req_json, sizeof(req_json)) != 0) {
        fprintf(stderr, "[TTS] build request failed (text too long?)\n");
        goto cleanup;
    }

    printf("[TTS] Sending request...\n");
    if (wsp_send(client, req_json, strlen(req_json)) != LWS_OK) {
        fprintf(stderr, "[TTS] wsp_send failed\n");
        goto cleanup;
    }

    /* 6. 等待合成完成（带超时）*/
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += TTS_TIMEOUT_SEC;

    pthread_mutex_lock(&ctx.lock);
    while (!ctx.done && !g_stop) {
        int ret = pthread_cond_timedwait(&ctx.cond, &ctx.lock, &deadline);
        if (ret != 0) {
            fprintf(stderr, "[TTS] Timeout after %d seconds\n", TTS_TIMEOUT_SEC);
            break;
        }
    }
    int done  = ctx.done;
    int err   = ctx.api_code;
    long bytes = ctx.total_bytes;
    pthread_mutex_unlock(&ctx.lock);

    if (done && err == 0) {
        printf("\n[TTS] Success! Wrote %ld bytes → %s\n\n", bytes, out_file);
        printf("  Play:    aplay -r 16000 -f S16_LE -c 1 %s\n", out_file);
        printf("  To WAV:  sox -t raw -r 16000 -e signed -b 16 -c 1 %s demo.wav\n\n", out_file);
    } else if (!done) {
        fprintf(stderr, "[TTS] Did not receive last frame\n");
    }

cleanup:
    fclose(ctx.pcm_file);
    wsp_pool_destroy(pool);
    pthread_mutex_destroy(&ctx.lock);
    pthread_cond_destroy(&ctx.cond);

    return (done && err == 0 && !g_stop) ? 0 : 1;
}
