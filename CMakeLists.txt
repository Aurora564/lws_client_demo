cmake_minimum_required(VERSION 3.10)
project(ws_client_demo VERSION 1.0.0 LANGUAGES C)

# 设置 C 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 编译器选项
add_compile_definitions(_POSIX_C_SOURCE=200809L)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O2")

# 路径配置
set(OUTPUT_DIR ${CMAKE_SOURCE_DIR}/output)
set(LWS_INCLUDE_DIR ${OUTPUT_DIR}/include)
set(LWS_LIB_DIR ${OUTPUT_DIR}/lib)
set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(BUILD_APP_DIR ${CMAKE_BINARY_DIR}/app)

# 查找 libwebsockets
find_library(LWS_LIBRARY websockets
    PATHS ${LWS_LIB_DIR}
    NO_DEFAULT_PATH
)

if(NOT LWS_LIBRARY)
    message(FATAL_ERROR "libwebsockets not found. Please run build_deps.sh first.")
endif()

# 查找必需的库
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

# ==================== 编译 wsclient (v1) 静态库 ====================
add_library(wsclient STATIC
    ${SRC_DIR}/ws_client.c
)

target_include_directories(wsclient PUBLIC
    ${LWS_INCLUDE_DIR}
    ${LWS_INCLUDE_DIR}/libwebsockets
    ${SRC_DIR}
)

target_link_libraries(wsclient PUBLIC
    ${LWS_LIBRARY}
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    m
)

# 设置输出目录
set_target_properties(wsclient PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${BUILD_APP_DIR}
)

# ==================== 编译 wsclientv2 静态库 ====================
add_library(wsclientv2 STATIC
    ${SRC_DIR}/ws_client_v2.c
)

target_include_directories(wsclientv2 PUBLIC
    ${LWS_INCLUDE_DIR}
    ${LWS_INCLUDE_DIR}/libwebsockets
    ${SRC_DIR}
)

target_link_libraries(wsclientv2 PUBLIC
    ${LWS_LIBRARY}
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    m
)

set_target_properties(wsclientv2 PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${BUILD_APP_DIR}
)

# ==================== 编译 wspoolclient 静态库 ====================
add_library(wspoolclient STATIC
    ${SRC_DIR}/ws_pool.c
)

target_include_directories(wspoolclient PUBLIC
    ${LWS_INCLUDE_DIR}
    ${LWS_INCLUDE_DIR}/libwebsockets
    ${SRC_DIR}
)

target_link_libraries(wspoolclient PUBLIC
    ${LWS_LIBRARY}
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    m
)

set_target_properties(wspoolclient PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${BUILD_APP_DIR}
)

# ==================== 编译 wsserver 静态库 ====================
add_library(wsserver STATIC
    ${SRC_DIR}/ws_server.c
)

target_include_directories(wsserver PUBLIC
    ${LWS_INCLUDE_DIR}
    ${LWS_INCLUDE_DIR}/libwebsockets
    ${SRC_DIR}
)

target_link_libraries(wsserver PUBLIC
    ${LWS_LIBRARY}
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    m
)

set_target_properties(wsserver PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${BUILD_APP_DIR}
)

# ==================== 编译示例程序（可选）====================
option(BUILD_EXAMPLES "Build example programs" ON)

if(BUILD_EXAMPLES)
    set(EXAMPLES_DIR ${CMAKE_SOURCE_DIR}/examples)

    # ws_client (1:1) 示例
    function(add_example name)
        add_executable(${name} ${EXAMPLES_DIR}/${name}.c)
        target_include_directories(${name} PRIVATE ${SRC_DIR})
        target_link_libraries(${name} PRIVATE wsclient)
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${BUILD_APP_DIR}
            INSTALL_RPATH "${LWS_LIB_DIR}"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    endfunction()

    # ws_client v2 (1:1) 示例
    function(add_v2_example name)
        add_executable(${name} ${EXAMPLES_DIR}/${name}.c)
        target_include_directories(${name} PRIVATE ${SRC_DIR})
        target_link_libraries(${name} PRIVATE wsclientv2)
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${BUILD_APP_DIR}
            INSTALL_RPATH "${LWS_LIB_DIR}"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    endfunction()

    # ws_pool (1:N) 示例
    function(add_pool_example name)
        add_executable(${name} ${EXAMPLES_DIR}/${name}.c)
        target_include_directories(${name} PRIVATE ${SRC_DIR})
        target_link_libraries(${name} PRIVATE wspoolclient)
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${BUILD_APP_DIR}
            INSTALL_RPATH "${LWS_LIB_DIR}"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    endfunction()

    # echo_server（测试用服务器）
    function(add_server_example name)
        add_executable(${name} ${EXAMPLES_DIR}/${name}.c)
        target_include_directories(${name} PRIVATE ${SRC_DIR})
        target_link_libraries(${name} PRIVATE wsserver)
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${BUILD_APP_DIR}
            INSTALL_RPATH "${LWS_LIB_DIR}"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    endfunction()

    add_example(example)
    add_example(asr_multi_session)

    add_pool_example(pool_basic)
    add_pool_example(pool_multi_client)
    add_pool_example(pool_asr_sessions)
    add_pool_example(xfyun_tts)

    add_v2_example(v2_hooks_demo)

    add_server_example(echo_server)
endif()

# ==================== 安装规则 ====================
install(TARGETS wsclient
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES
    ${SRC_DIR}/ws_client.h
    DESTINATION include
)

# 生成 pkg-config 文件
configure_file(
    ${CMAKE_SOURCE_DIR}/wsclient.pc.in
    ${CMAKE_BINARY_DIR}/wsclient.pc
    @ONLY
)

install(FILES
    ${CMAKE_BINARY_DIR}/wsclient.pc
    DESTINATION lib/pkgconfig
)

# ==================== 打印配置信息 ====================
message(STATUS "")
message(STATUS "========================================")
message(STATUS "  Configuration Summary")
message(STATUS "========================================")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install prefix:    ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Output dir:        ${OUTPUT_DIR}")
message(STATUS "  Build examples:    ${BUILD_EXAMPLES}")
message(STATUS "========================================")
message(STATUS "")
