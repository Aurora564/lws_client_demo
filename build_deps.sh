#!/bin/bash
# 编译 libwebsockets，输出到项目本地 output/ 目录，不污染系统
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LWS_SRC="$SCRIPT_DIR/libs/libwebsockets"
BUILD_DIR="$SCRIPT_DIR/build/libwebsockets"
INSTALL_DIR="$SCRIPT_DIR/output"

echo "[1/3] 准备构建目录..."
mkdir -p "$BUILD_DIR"
mkdir -p "$INSTALL_DIR"

echo "[2/3] CMake 配置 libwebsockets..."
cmake -S "$LWS_SRC" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DLWS_WITH_SHARED=ON \
    -DLWS_WITH_STATIC=ON \
    -DLWS_WITH_SSL=ON \
    -DLWS_WITH_ZLIB=ON \
    -DLWS_WITH_LIBUV=OFF \
    -DLWS_WITH_LIBEVENT=OFF \
    -DLWS_WITH_LIBEV=OFF \
    -DLWS_WITHOUT_TESTAPPS=ON \
    -DLWS_WITHOUT_TEST_SERVER=ON \
    -DLWS_WITHOUT_TEST_PING=ON \
    -DLWS_WITHOUT_TEST_CLIENT=ON \
    -DLWS_WITH_MINIMAL_EXAMPLES=OFF \
    -DDISABLE_WERROR=ON

echo "[3/3] 编译并安装到 $INSTALL_DIR ..."
cmake --build "$BUILD_DIR" --parallel "$(nproc)"
cmake --install "$BUILD_DIR"

echo ""
echo "完成！输出目录结构："
echo "  头文件: $INSTALL_DIR/include/"
echo "  库文件: $INSTALL_DIR/lib/"
echo ""
echo "编译你的项目时使用："
echo "  CFLAGS : -I$INSTALL_DIR/include"
echo "  LDFLAGS: -L$INSTALL_DIR/lib -lwebsockets"
echo "  运行时 : LD_LIBRARY_PATH=$INSTALL_DIR/lib:\$LD_LIBRARY_PATH"
