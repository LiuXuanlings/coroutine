# 针对 tests 目录的 CMake 配置

# MC-603 留存清单中的测试源文件，禁止通过 GLOB 隐式扩大测试边界。
set(TEST_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_autodrive_components.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_autodrive_proto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_atomic_rw_lock.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_base_macros.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_cache_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_channel_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_choreography_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_classic_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_common_types.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_component.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_component_base.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_component_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_condition_notifier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_control_sink_metrics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_croutine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data_fusion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data_notifier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data_visitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_fusion_all_latest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_hybrid_transport.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/test_routine_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler_choreography.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler_classic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_shm_block.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_shm_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_shm_segment.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_shm_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_smoke.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_time.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_topology.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_topology_discovery.cpp
)

# 如果没有测试文件，则跳过
add_library(test_module_controller_plugin SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/module_controller_test_plugin.cpp)
target_link_libraries(test_module_controller_plugin PRIVATE minicyber_core)

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
        if(TEST_NAME STREQUAL "test_module_controller")
            add_dependencies(${TEST_NAME} mainboard test_module_controller_plugin)
            target_compile_definitions(${TEST_NAME} PRIVATE
                MINICYBER_TEST_PLUGIN_PATH="$<TARGET_FILE:test_module_controller_plugin>"
                MINICYBER_MAINBOARD_PATH="$<TARGET_FILE:mainboard>")
        endif()
        if(TEST_NAME MATCHES "^test_autodrive_")
            if(TEST_NAME STREQUAL "test_autodrive_components")
                add_dependencies(${TEST_NAME} minicyber_autodrive_components)
            endif()
            target_link_libraries(${TEST_NAME} minicyber_autodrive_runtime)
        endif()
        if(TEST_NAME STREQUAL "test_autodrive_components")
            target_compile_definitions(${TEST_NAME} PRIVATE
                MINICYBER_AUTODRIVE_COMPONENTS_PATH="$<TARGET_FILE:minicyber_autodrive_components>"
                MINICYBER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        endif()
        if(TEST_NAME STREQUAL "test_control_sink_metrics")
            target_link_libraries(${TEST_NAME} minicyber_autodrive_metrics)
        endif()
        
        # 注册到 ctest
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
        set(TEST_LABELS "unit")
        if(TEST_NAME MATCHES "^test_(condition_notifier|posix_segment|posix_segment_signal|shm_dispatcher)$")
            list(APPEND TEST_LABELS "cross_process")
        endif()
        if(TEST_NAME MATCHES "^test_(condition_notifier|control_sink_metrics|pin_thread|posix_segment_signal)$")
            list(APPEND TEST_LABELS "high_risk")
        endif()
        if(TEST_NAME MATCHES "^test_topology$")
            list(APPEND TEST_LABELS "high_risk")
        endif()
        if(TEST_NAME MATCHES "^test_topology_discovery$")
            list(APPEND TEST_LABELS "high_risk" "cross_process")
        endif()
        if(TEST_NAME MATCHES "^test_hybrid_transport$")
            list(APPEND TEST_LABELS "high_risk" "cross_process")
        endif()
        if(TEST_NAME MATCHES "^test_node$")
            list(APPEND TEST_LABELS "high_risk")
        endif()
        if(TEST_NAME MATCHES "^test_(croutine|data_dispatcher|data_notifier|data_visitor|data_fusion|fusion_all_latest|routine_factory|processor|scheduler_classic)$")
            list(APPEND TEST_LABELS "high_risk")
        endif()
        if(TEST_NAME MATCHES "^test_component$")
            list(APPEND TEST_LABELS "high_risk")
        endif()
        if(TEST_NAME STREQUAL "test_module_controller")
            list(APPEND TEST_LABELS "high_risk" "integration")
        endif()
        if(TEST_NAME STREQUAL "test_autodrive_components")
            list(APPEND TEST_LABELS "high_risk" "integration")
        endif()
        if(TEST_NAME MATCHES "^test_scheduler_choreography$")
            list(APPEND TEST_LABELS "high_risk")
        endif()
        set_tests_properties(${TEST_NAME} PROPERTIES LABELS "${TEST_LABELS}")
    endforeach()
endif()

add_custom_target(check_cross_process
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L cross_process
    DEPENDS test_condition_notifier test_posix_segment test_posix_segment_signal test_shm_dispatcher test_topology_discovery test_hybrid_transport
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# MC-618 的业务视角验收只驱动唯一自动驾驶 DAG：真实 mainboard dlopen、跨进程
# SHM 端点、组件内 INTRA 链和 Classic CRoutine 都由统一脚本的进程日志及 Sink
# 结果证明。独立目录保留失败现场，不能以单元测试替代这条 1000 条主干。
add_test(NAME test_autodrive_classic_pipeline
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_autodrive_classic_pipeline.sh
                 ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})
set_tests_properties(test_autodrive_classic_pipeline PROPERTIES
    LABELS "integration;high_risk;cross_process"
    TIMEOUT 50)

# MC-619 沿用 Classic 的唯一脚本，仅替换独立 Scheduler 配置。驱动同时校验
# 定向区/公共池线程、Control 同一 Writer 的 INTRA+SHM 扇出和逐文件触达报告。
add_test(NAME test_autodrive_choreography_pipeline
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DBINARY_DIR=${CMAKE_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/autodrive_pipeline_test.cmake)
set_tests_properties(test_autodrive_choreography_pipeline PROPERTIES
    LABELS "integration;high_risk;cross_process"
    TIMEOUT 50)
