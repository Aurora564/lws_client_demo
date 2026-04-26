/**
 * ws_internal.h - ws_client_v2 / ws_pool_v2 共享内部数据结构
 *
 * 优化点:
 *   - ws_msg_node_t: 柔性数组成员 (flexible array member), 单次 malloc/free
 *   - ws_frag_buf_t: 分片缓冲抽象, 通用 frag_append 操作
 *   - 内联函数: 避免两个 .c 文件重复代码
 */
#pragma once
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 * 发送队列节点 (柔性数组成员 — 单次分配)
 *
 *   buf[] 的布局: [LWS_PRE 填充字节] [payload]
 *   len = payload 有效字节数 (不含 LWS_PRE)
 *
 *   分配: ws_msg_node_t *n = malloc(sizeof(*n) + LWS_PRE + len);
 *   释放: free(n);
 * ==================================================================== */
typedef struct ws_msg_node {
    size_t                len;
    int                   is_binary;
    struct ws_msg_node   *next;
    unsigned char         buf[];    /* 柔性数组成员 */
} ws_msg_node_t;

/* ====================================================================
 * 分片缓冲抽象
 *
 *   嵌入到 wsl_client / wsp_client 结构体中,
 *   通过 ws_frag_append() 进行统一的分片追加操作。
 * ==================================================================== */
typedef struct {
    unsigned char  *buf;
    size_t          len;
    size_t          cap;
} ws_frag_buf_t;

/* ------------------------------------------------------------------ */
/* 内联工具函数                                                       */
/* ------------------------------------------------------------------ */

/* 创建消息节点, 单次 malloc, prefix 通常为 LWS_PRE */
static inline ws_msg_node_t *ws_msg_new(const void *data, size_t len,
                                         int is_binary, size_t prefix)
{
    ws_msg_node_t *n = (ws_msg_node_t *)malloc(sizeof(*n) + prefix + len);
    if (!n) return NULL;
    memcpy(n->buf + prefix, data, len);
    n->len      = len;
    n->is_binary = is_binary;
    n->next     = NULL;
    return n;
}

/* 分片追加, OOM 返回 -1 */
static inline int ws_frag_append(ws_frag_buf_t *f,
                                  const unsigned char *data, size_t len)
{
    if (f->len + len > f->cap) {
        size_t new_cap = f->cap ? f->cap : 4096;
        while (new_cap < f->len + len)
            new_cap *= 2;
        unsigned char *p = (unsigned char *)realloc(f->buf, new_cap);
        if (!p) return -1;
        f->buf = p;
        f->cap = new_cap;
    }
    memcpy(f->buf + f->len, data, len);
    f->len += len;
    return 0;
}

/* 清空消息队列 */
static inline void ws_flush_queue(ws_msg_node_t **head, ws_msg_node_t **tail,
                                   size_t *count, size_t *bytes)
{
    ws_msg_node_t *n = *head;
    while (n) {
        ws_msg_node_t *next = n->next;
        free(n);
        n = next;
    }
    *head = *tail = NULL;
    if (count) *count = 0;
    if (bytes) *bytes = 0;
}

#ifdef __cplusplus
}
#endif
