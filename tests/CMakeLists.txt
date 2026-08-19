# 针对 tests 目录的 CMake 配置

#  留存清单中的测试源文件，禁止通过 GLOB 隐式扩大测试边界。
set(TEST_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_atomic_hash_map.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_atomic_rw_lock.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_base_macros.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_bounded_queue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_cache_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_channel_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_choreography_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_classic_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_common_types.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_component.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_component_base.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_component_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_condition_notifier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_croutine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data_fusion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data_notifier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data_visitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_fusion_all_latest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_intra_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_intra_receiver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_intra_transmitter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_module_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_node.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_pin_thread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_posix_segment.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_posix_segment_signal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_processor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_processor_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler_choreography.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler_classic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_shm_block.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_shm_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_shm_receiver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_shm_segment.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_shm_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_shm_transmitter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_smoke.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_time.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_topology.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_topology_discovery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_transport_routing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_wait_strategy.cpp
)

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
        if(TEST_NAME MATCHES "^test_topology$")
            list(APPEND TEST_LABELS "high_risk")
        endif()
        if(TEST_NAME MATCHES "^test_topology_discovery$")
            list(APPEND TEST_LABELS "high_risk" "cross_process")
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
    DEPENDS test_condition_notifier test_posix_segment test_posix_segment_signal test_shm_dispatcher test_shm_transmitter test_topology_discovery
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
