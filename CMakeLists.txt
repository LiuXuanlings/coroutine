cmake_minimum_required(VERSION 3.14)
# Compile preprocessed assembly through the C compiler driver. CMake's direct
# GNU as invocation converts shared-library export macros to invalid --defsym
# arguments for src/swap.S.
set(CMAKE_ASM_COMPILER /usr/bin/cc CACHE FILEPATH "ASM compiler")
project(minicyber CXX C ASM)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------
# 编译选项配置 (包含 Sanitizers)
# ---------------------------------------------------------
# 默认开启较严格的警告。优化和调试信息由 CMAKE_BUILD_TYPE 决定，
# 不再把 -O0 -g 注入 Release 或 sanitizer 构建。
add_compile_options(-Wall -Wextra)

# 允许通过 CMake 参数选择 Sanitizer类型: 
# cmake -DUSE_SANITIZER=ASAN (内存检测) 或 TSAN (线程安全检测)
set(USE_SANITIZER "" CACHE STRING "Compile with ASAN or TSAN")
set_property(CACHE USE_SANITIZER PROPERTY STRINGS "" ASAN TSAN)

if(USE_SANITIZER STREQUAL "ASAN")
    message(STATUS "Enable AddressSanitizer (ASan)")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
elseif(USE_SANITIZER STREQUAL "TSAN")
    message(STATUS "Enable ThreadSanitizer (TSan)")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT USE_SANITIZER STREQUAL "")
    message(FATAL_ERROR "USE_SANITIZER must be ASAN, TSAN, or empty")
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
find_package(FastRTPS REQUIRED CONFIG NAMES FastRTPS fastrtps)
if(NOT TARGET fastrtps)
    message(FATAL_ERROR "FastRTPS was found, but it did not export the fastrtps target")
endif()
# 将protobuf头文件路径加入全局头文件搜索路径
include_directories(${Protobuf_INCLUDE_DIRS})

# Proto 文件编译产物输出目录
# CMAKE_CURRENT_BINARY_DIR = build构建目录
set(PROTO_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)

# MC-603 留存清单中的 Proto 源文件，禁止递归 GLOB 将黑名单重新编入。
set(PROTO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/minicyber/proto/autodrive.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/include/minicyber/proto/component_conf.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/include/minicyber/proto/dag_conf.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/include/minicyber/proto/qos_profile.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/include/minicyber/proto/role_attributes.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/include/minicyber/proto/scheduler_conf.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/include/minicyber/proto/topology_change.proto
)

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
set(MINICYBER_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/component/component_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/croutine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/croutine/detail/routine_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/data/data_notifier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mainboard/module_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service_discovery/channel_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler/common/pin_thread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler/policy/choreography_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler/policy/classic_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler/processor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler/processor_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler/scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler/scheduler_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/swap.S
    ${CMAKE_CURRENT_SOURCE_DIR}/src/time/duration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/time/rate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/time/time.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/topology/topology_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transport/dispatcher/shm_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transport/shm/condition_notifier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transport/shm/posix_segment.cpp
)
add_library(minicyber_core SHARED ${MINICYBER_CORE_SOURCES})
target_link_libraries(minicyber_core
    PUBLIC minicyber_proto ${Protobuf_LIBRARIES} fastrtps
    PRIVATE pthread dl rt
)
# 业务 DSO 不能按需从静态 Proto 库复制尚未被核心引用的消息描述符（MC-615）。
# 核心库作为唯一 ABI 所有者完整收纳生成对象，插件只解析此处导出的符号。
target_link_options(minicyber_core PRIVATE
    "-Wl,--whole-archive,$<TARGET_FILE:minicyber_proto>,--no-whole-archive"
)

# 自动驾驶消息的模板实例和 shared_ptr 控制块需要一个先于组件
# 插件加载、后于插件卸载的常驻所有者。它属于 Demo 运行时，不能让
# 通用 minicyber_core 依赖 CameraFrame/ControlCommand 等具体业务类型。
add_library(minicyber_autodrive_runtime SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/demo/autodrive/autodrive_runtime.cpp)
target_link_libraries(minicyber_autodrive_runtime PRIVATE minicyber_core)

# MC-615 的唯一业务插件。业务 Component 只能经 ModuleController 的 dlopen
# 触发静态注册，不能链接进 mainboard 绕过 CyberRT ClassLoader 对应的插件边界。
set(AUTODRIVE_COMPONENT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/demo/autodrive/components/control_audit_component.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/demo/autodrive/components/control_component.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/demo/autodrive/components/fusion_component.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/demo/autodrive/components/perception_component.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/demo/autodrive/components/planning_component.cpp
)
add_library(minicyber_autodrive_components SHARED ${AUTODRIVE_COMPONENT_SOURCES})
target_link_libraries(minicyber_autodrive_components
    PRIVATE minicyber_autodrive_runtime minicyber_core
)
# 业务插件含模板传输代码；GCC 默认的 GNU unique 局部静态对象会让 glibc
# 将该 DSO 视为 NODELETE，从而跳过静态注册器的析构和工厂注销。核心库继续
# 持有跨 DSO 的运行时单例，插件本身必须允许 ModuleController::Clear dlclose。
target_compile_options(minicyber_autodrive_components PRIVATE -fno-gnu-unique)

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

# MC-616 的唯一跨进程业务两端。它们只连接 MC-615 已冻结的频道和 DAG，
# 不把 Source/Sink 混入可 dlopen 的组件库，保留 CyberRT 独立 Node 进程边界。
add_executable(sensor_source demo/autodrive/sensor_source.cpp)
target_link_libraries(sensor_source PRIVATE minicyber_autodrive_runtime
                    minicyber_core pthread dl)
set_target_properties(sensor_source PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(control_sink demo/autodrive/control_sink.cpp)
target_link_libraries(control_sink PRIVATE minicyber_autodrive_runtime
                    minicyber_core pthread dl)
set_target_properties(control_sink PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------
# 添加测试子目录
# ---------------------------------------------------------
# MC-613 的真实 dlopen 测试插件需要引用 mainboard 的目标文件路径，因此
# mainboard 必须在 tests 子目录之前声明。
enable_testing()
add_subdirectory(tests)
