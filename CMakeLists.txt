cmake_minimum_required(VERSION 3.14)
project(sylar_rewrite CXX C ASM)

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
# 构建核心库 
# ---------------------------------------------------------
file(GLOB_RECURSE SRC_FILES "src/*.cpp" "src/*.S")
add_library(sylar_core SHARED ${SRC_FILES})
target_link_libraries(sylar_core pthread dl)

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

