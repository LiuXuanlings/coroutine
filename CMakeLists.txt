cmake_minimum_required(VERSION 3.14)
# Compile preprocessed assembly through the C compiler driver. CMake's direct
# GNU as invocation converts shared-library export macros to invalid --defsym
# arguments for src/swap.S.
set(CMAKE_ASM_COMPILER /usr/bin/cc CACHE FILEPATH "ASM compiler")
project(minicyber CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------
# 编译选项配置 (包含 Sanitizers)
# ---------------------------------------------------------
# 默认开启较严格的警告
add_compile_options(-Wall -Wextra -g -O0)

# 允许通过 CMake 参数选择 Sanitizer类型: 
# cmake -DUSE_SANITIZER=ASAN (内存检测) 或 TSAN (线程安全检测)
set(USE_SANITIZER "" CACHE STRING "Compile with ASAN or TSAN")

if(USE_SANITIZER STREQUAL "ASAN")
    message(STATUS "Enable AddressSanitizer (ASan)")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
elseif(USE_SANITIZER STREQUAL "TSAN")
    message(STATUS "Enable ThreadSanitizer (TSan)")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# ---------------------------------------------------------
# 包含目录
# ---------------------------------------------------------
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------
# Protobuf 支持
# ---------------------------------------------------------
# 查找系统安装的Protobuf库，找不到则直接报错（REQUIRED）
find_package(Protobuf REQUIRED)
# 将protobuf头文件路径加入全局头文件搜索路径
include_directories(${Protobuf_INCLUDE_DIRS})

# Proto 文件编译产物输出目录
# CMAKE_CURRENT_BINARY_DIR = build构建目录
set(PROTO_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)

# 递归搜集所有proto文件：include/minicyber/proto/下全部*.proto
file(GLOB_RECURSE PROTO_FILES "include/minicyber/proto/*.proto")

# 循环遍历每一个proto文件
foreach(PROTO_FILE ${PROTO_FILES})
    # 计算相对路径
    file(RELATIVE_PATH PROTO_REL ${CMAKE_CURRENT_SOURCE_DIR}/include ${PROTO_FILE})
    get_filename_component(PROTO_DIR ${PROTO_REL} DIRECTORY)
    # 获取文件名（不带后缀）：qos_profile
    get_filename_component(PROTO_NAME ${PROTO_REL} NAME_WE)

    # 拼接生成的 .pb.cc 文件完整路径，存入PROTO_SRCS列表
    list(APPEND PROTO_SRCS ${PROTO_BINARY_DIR}/${PROTO_DIR}/${PROTO_NAME}.pb.cc)

    # 自定义命令：调用protoc工具把proto翻译成C++代码
    add_custom_command(
        # 命令输出产物：生成的头文件+源文件
        OUTPUT ${PROTO_BINARY_DIR}/${PROTO_DIR}/${PROTO_NAME}.pb.cc
               ${PROTO_BINARY_DIR}/${PROTO_DIR}/${PROTO_NAME}.pb.h
        # 先创建输出文件夹
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_BINARY_DIR}
        # 执行protoc编译器
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --cpp_out=${PROTO_BINARY_DIR}    # C++代码输出根目录
            -I ${CMAKE_CURRENT_SOURCE_DIR}/include  # proto import搜索根目录
            ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}  # 依赖proto源码，proto修改后自动重新编译
        COMMENT "Compiling ${PROTO_REL}"
    )
endforeach()

# 将proto代码生成目录加入头文件搜索路径
include_directories(${PROTO_BINARY_DIR})

# 将所有生成的pb.cc编译为【静态库 minicyber_proto】
# POSITION_INDEPENDENT_CODE ON: 因为 minicyber_proto 是静态库，会被链接进
# libminicyber_core.so (SHARED)。静态库目标必须开启 -fPIC，否则在链接为
# 共享库时会出现 "relocation R_X86_64_PC32 can not be used when making a
# shared object; recompile with -fPIC" 错误。
add_library(minicyber_proto STATIC ${PROTO_SRCS})
set_target_properties(minicyber_proto PROPERTIES POSITION_INDEPENDENT_CODE ON)
# 链接protobuf运行时库
target_link_libraries(minicyber_proto ${Protobuf_LIBRARIES})
# 对外暴露头文件路径，其他target链接minicyber_proto时自动继承
target_include_directories(minicyber_proto
    PUBLIC ${PROTO_BINARY_DIR}
    PUBLIC ${Protobuf_INCLUDE_DIRS}
)

# ---------------------------------------------------------
# 构建核心库
# ---------------------------------------------------------
file(GLOB_RECURSE SRC_FILES "src/*.cpp" "src/*.S")
add_library(minicyber_core SHARED ${SRC_FILES})
target_link_libraries(minicyber_core minicyber_proto ${Protobuf_LIBRARIES} pthread dl rt)

# ---------------------------------------------------------
# 集成 GoogleTest (使用 FetchContent 自动下载)
# ---------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://kkgithub.com/google/googletest.git
  GIT_TAG        release-1.12.1 # 稳定版本
)
# 对于Windows，强制使用动态运行库以防冲突，Linux下无影响
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---------------------------------------------------------
# 添加测试子目录
# ---------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------
# 示例程序 (examples/)
# ---------------------------------------------------------
file(GLOB EXAMPLE_FILES "examples/*.cpp")
foreach(EXAMPLE_SRC ${EXAMPLE_FILES})
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_SRC} NAME_WE)
    add_executable(${EXAMPLE_NAME} ${EXAMPLE_SRC})
    target_link_libraries(${EXAMPLE_NAME} minicyber_core pthread dl)
endforeach()

# ---------------------------------------------------------
# mainboard 启动入口 (bin/)
# ---------------------------------------------------------
# mainboard 是 MiniCyber 的标准化启动程序，读取 DAG 配置并通过 dlopen 加载
# 业务组件 .so。链接 minicyber_proto 是因为 bin/mainboard.cpp 直接包含了
# module_controller.h，后者依赖 dag_conf.pb.h。
# 输出到 build/bin/ 下，便于与 examples 分离。
# ---------------------------------------------------------
add_executable(mainboard bin/mainboard.cpp)
target_link_libraries(mainboard minicyber_core minicyber_proto
                      ${Protobuf_LIBRARIES} pthread dl)
set_target_properties(mainboard PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
