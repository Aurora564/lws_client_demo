#!/bin/bash
# WebSocket 客户端项目编译脚本
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
OUTPUT_DIR="$SCRIPT_DIR/output"
BUILD_DIR="$SCRIPT_DIR/build/app"

# 颜色输出
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  WebSocket Client Build Script${NC}"
echo -e "${GREEN}========================================${NC}"

# 检查依赖是否已构建
if [ ! -f "$OUTPUT_DIR/lib/libwebsockets.a" ] && [ ! -f "$OUTPUT_DIR/lib/libwebsockets.so" ]; then
    echo -e "${YELLOW}[警告] libwebsockets 未编译，正在执行 build_deps.sh...${NC}"
    if [ -f "$SCRIPT_DIR/build_deps.sh" ]; then
        bash "$SCRIPT_DIR/build_deps.sh"
    else
        echo -e "${RED}[错误] 找不到 build_deps.sh，请先编译 libwebsockets${NC}"
        exit 1
    fi
fi

# 创建构建目录
mkdir -p "$BUILD_DIR"

# 编译器设置
CC=${CC:-gcc}
CFLAGS="-Wall -Wextra -O2 -g"
CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L"

# 包含路径
INCLUDES="-I$OUTPUT_DIR/include"
INCLUDES="$INCLUDES -I$OUTPUT_DIR/include/libwebsockets"
INCLUDES="$INCLUDES -I$SRC_DIR"

# 库路径和链接选项
LDFLAGS="-L$OUTPUT_DIR/lib"
LIBS="-lwebsockets -lpthread -lssl -lcrypto -lz -lm"

# 检测是否需要 rpath
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    LDFLAGS="$LDFLAGS -Wl,-rpath,$OUTPUT_DIR/lib"
fi

echo ""
echo "[1/2] 编译 ws_client.c 为静态库..."
$CC $CFLAGS $INCLUDES -c "$SRC_DIR/ws_client.c" -o "$BUILD_DIR/ws_client.o"
ar rcs "$BUILD_DIR/libwsclient.a" "$BUILD_DIR/ws_client.o"
rm -f "$BUILD_DIR/ws_client.o"

echo "[2/2] 生成 pkg-config 文件..."
cat > "$OUTPUT_DIR/lib/pkgconfig/wsclient.pc" << EOF
prefix=$OUTPUT_DIR
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: wsclient
Description: WebSocket Client Library based on libwebsockets
Version: 1.0.0
Libs: -L\${libdir} -lwsclient -lwebsockets -lpthread -lssl -lcrypto -lz -lm
Cflags: -I\${includedir} -I\${includedir}/libwebsockets -D_POSIX_C_SOURCE=200809L
EOF

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  编译成功！${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "生成的库文件:"
echo "  静态库: $BUILD_DIR/libwsclient.a"
echo ""
echo "使用示例 (在您的项目中):"
echo ""
echo "方法1 - 直接链接:"
echo "  gcc your_app.c -I$OUTPUT_DIR/include \\"
echo "      -I$OUTPUT_DIR/include/libwebsockets \\"
echo "      -L$BUILD_DIR -lwsclient \\"
echo "      -L$OUTPUT_DIR/lib -lwebsockets \\"
echo "      -lpthread -lssl -lcrypto -lz -lm"
echo ""
echo "方法2 - 使用 pkg-config:"
echo "  export PKG_CONFIG_PATH=$OUTPUT_DIR/lib/pkgconfig:\$PKG_CONFIG_PATH"
echo "  gcc your_app.c \$(pkg-config --cflags --libs wsclient)"
echo ""
echo "运行时的环境变量:"
echo "  export LD_LIBRARY_PATH=$OUTPUT_DIR/lib:\$LD_LIBRARY_PATH"
echo ""
