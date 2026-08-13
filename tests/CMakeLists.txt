# 针对 tests 目录的 CMake 配置

# 查找 tests 目录下所有的 GTest 测试文件（约定以 test_ 开头）。
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
        set(TEST_LABELS "unit")
        if(TEST_NAME MATCHES "^test_scheduler_")
            list(APPEND TEST_LABELS "scheduler" "stability" "sanitizer")
        endif()
        if(TEST_NAME MATCHES "^test_(atomic_hash_map|atomic_rw_lock|bounded_queue|shm_block|shm_state)$")
            list(APPEND TEST_LABELS "stress")
        endif()
        if(TEST_NAME MATCHES "^test_(condition_notifier|posix_segment|posix_segment_signal|shm_dispatcher|shm_transmitter)$")
            list(APPEND TEST_LABELS "cross_process")
        endif()
        set_tests_properties(${TEST_NAME} PROPERTIES LABELS "${TEST_LABELS}")
    endforeach()
endif()

add_custom_target(check_scheduler_tsan
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L sanitizer
    DEPENDS test_scheduler_classic test_scheduler_choreography
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_custom_target(check_stress
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L stress
    DEPENDS test_atomic_hash_map test_atomic_rw_lock test_bounded_queue test_shm_block test_shm_state
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_custom_target(check_cross_process
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L cross_process
    DEPENDS test_condition_notifier test_posix_segment test_posix_segment_signal test_shm_dispatcher test_shm_transmitter
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
