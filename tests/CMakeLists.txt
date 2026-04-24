# 针对 tests 目录的 CMake 配置

# 查找 tests 目录下所有的 GTest 测试文件 (约定以 test_ 开头，比如 test_fiber.cpp)
file(GLOB TEST_FILES "test_*.cpp")

# 如果没有测试文件，则跳过
if(TEST_FILES)
    # 为每个 .cpp 文件生成一个独立的可执行测试程序
    foreach(TEST_SRC ${TEST_FILES})
        get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
        add_executable(${TEST_NAME} ${TEST_SRC})
        
        # 链接 gtest 和 核心库 (当前核心库被注释掉了，所以只链接 gtest)
        # 还要链接 pthread，因为 gtest 和我们的框架都需要
        target_link_libraries(${TEST_NAME} 
            gtest_main 
            gtest 
            pthread 
            dl
            minicyber_core
        )
        
        # 注册到 ctest
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endforeach()
endif()

# 单独处理独立的测试/演示文件 (不依赖 gtest)
# 为什么前面的 file(GLOB "*.cpp") 用相对路径没问题，而这里的 if(EXISTS) 却不行？
# 因为 CMake 对相对路径的解析基准是"分裂"的：构建声明 (add_executable) 和源码搜索 (file(GLOB)) 默认基于当前源码目录；而底层文件系统操作 (if(EXISTS), file(READ)) 默认基于当前的执行目录 (即 build 目录)。因此，为了避免去 build 目录找源码导致误判，在使用 if(EXISTS) 时必须强制拼接 ${CMAKE_CURRENT_SOURCE_DIR} 转换为绝对路径。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/example.cpp")
    add_executable(example example.cpp)
    target_link_libraries(example minicyber_core pthread dl)
endif()