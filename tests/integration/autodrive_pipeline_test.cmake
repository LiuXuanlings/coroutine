if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "SOURCE_DIR and BINARY_DIR are required")
endif()

string(RANDOM LENGTH 12 ALPHABET "0123456789abcdef" run_id)
set(run_dir "${BINARY_DIR}/autodrive-choreography-ctest-${run_id}")
execute_process(
  COMMAND "${SOURCE_DIR}/scripts/run_autodrive_pipeline.sh"
          --build-dir "${BINARY_DIR}"
          --scheduler "${SOURCE_DIR}/config/autodrive/choreo_sched.conf"
          --output-dir "${run_dir}"
          --messages 1000 --frequency 100 --metrics
  WORKING_DIRECTORY "${SOURCE_DIR}"
  RESULT_VARIABLE pipeline_result
  OUTPUT_VARIABLE pipeline_output
  ERROR_VARIABLE pipeline_error)
if(NOT pipeline_result EQUAL 0)
  message(FATAL_ERROR "Choreography pipeline failed (${pipeline_result}):\n${pipeline_output}\n${pipeline_error}")
endif()

function(require_value file key out_var)
  file(STRINGS "${file}" lines)
  set(value "")
  foreach(line IN LISTS lines)
    if(line MATCHES "^${key}=(.*)$")
      set(value "${CMAKE_MATCH_1}")
      break()
    endif()
  endforeach()
  if(value STREQUAL "")
    message(FATAL_ERROR "Missing ${key} in ${file}")
  endif()
  set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

set(metrics_file "${run_dir}/metrics.txt")
set(mainboard_log "${run_dir}/mainboard.log")
set(mainboard_evidence "${run_dir}/mainboard_evidence.txt")
set(sink_evidence "${run_dir}/control_sink_evidence.txt")
foreach(required_file IN ITEMS "${metrics_file}" "${mainboard_log}"
                              "${run_dir}/sensor_source.log"
                              "${run_dir}/control_sink.log"
                              "${mainboard_evidence}" "${sink_evidence}")
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR "Missing pipeline evidence: ${required_file}")
  endif()
endforeach()

file(READ "${metrics_file}" metrics)
if(NOT metrics MATCHES "METRICS received=1000 audit_received=1000 lost=0 duplicates=0 audit_duplicates=0 out_of_order=0 audit_difference=0")
  message(FATAL_ERROR "Unexpected Choreography metrics: ${metrics}")
endif()
file(READ "${mainboard_log}" mainboard)
foreach(component IN ITEMS PerceptionComponent FusionComponent PlanningComponent
                          ControlComponent ControlAuditComponent)
  if(NOT mainboard MATCHES "Component loaded: ${component}")
    message(FATAL_ERROR "mainboard did not load ${component}")
  endif()
endforeach()

set(expected_values "")
foreach(sequence RANGE 1 1000)
  list(APPEND expected_values "${sequence}")
endforeach()
string(JOIN "," expected_sequences ${expected_values})

foreach(component IN ITEMS perception fusion planning control control_audit)
  require_value("${mainboard_evidence}" "component_${component}_sequences" sequences)
  if(NOT sequences STREQUAL expected_sequences)
    message(FATAL_ERROR "${component} did not process the complete measurement set")
  endif()
endforeach()
require_value("${mainboard_evidence}" "choreography_processor_0_tid" choreography_tid)
require_value("${mainboard_evidence}" "pool_processor_0_tid" pool_tid)
if(choreography_tid STREQUAL "-1" OR pool_tid STREQUAL "-1" OR choreography_tid STREQUAL pool_tid)
  message(FATAL_ERROR "Choreography directed and Classic pool Processor evidence is invalid")
endif()
require_value("${mainboard_evidence}" "component_perception_tid" perception_tid)
if(NOT perception_tid STREQUAL choreography_tid)
  message(FATAL_ERROR "Perception did not run on the directed Processor")
endif()
foreach(component IN ITEMS fusion planning control control_audit)
  require_value("${mainboard_evidence}" "component_${component}_tid" component_tid)
  if(NOT component_tid STREQUAL pool_tid)
    message(FATAL_ERROR "${component} did not run in the Classic pool")
  endif()
endforeach()
foreach(key IN ITEMS control_sequences control_audit_sequences)
  require_value("${mainboard_evidence}" "${key}" sequences)
  if(NOT sequences STREQUAL expected_sequences)
    message(FATAL_ERROR "Mainboard ${key} differs from the expected set")
  endif()
endforeach()
foreach(key IN ITEMS control_intra_enabled_count control_shm_enabled_count intra_pointer_identity_count)
  require_value("${mainboard_evidence}" "${key}" count)
  if(NOT count STREQUAL "1000")
    message(FATAL_ERROR "Mainboard ${key} is ${count}, expected 1000")
  endif()
endforeach()
require_value("${mainboard_evidence}" "mainboard_pid" mainboard_pid)
require_value("${sink_evidence}" "sink_pid" sink_pid)
if(mainboard_pid STREQUAL sink_pid)
  message(FATAL_ERROR "ControlSink must be a distinct SHM process")
endif()
foreach(key IN ITEMS control_sequences audit_sequences)
  require_value("${sink_evidence}" "${key}" sequences)
  if(NOT sequences STREQUAL expected_sequences)
    message(FATAL_ERROR "ControlSink ${key} differs from the expected set")
  endif()
endforeach()
require_value("${sink_evidence}" "control_first_pointer" sink_control_pointer)
require_value("${mainboard_evidence}" "control_first_pointer" mainboard_control_pointer)
if(sink_control_pointer STREQUAL "0" OR mainboard_control_pointer STREQUAL "0")
  message(FATAL_ERROR "Missing INTRA or SHM message object evidence")
endif()

# 触达报告同时核对冻结台账、当前 Git 生产路径和显式构建/运行时加载证据。
set(report "${run_dir}/file_touch_report.txt")
file(READ "${SOURCE_DIR}/docs/refactor/00_进度记录.md" ledger)
if(NOT ledger MATCHES "MC-603 冻结留存清单" OR
   NOT ledger MATCHES "MC-619 留存清单增删记录")
  message(FATAL_ERROR "Missing production manifest reconciliation records")
endif()
execute_process(
  COMMAND git ls-files -- bin demo/autodrive include/minicyber src config/autodrive
          scripts/run_autodrive_pipeline.sh CMakeLists.txt
  WORKING_DIRECTORY "${SOURCE_DIR}"
  RESULT_VARIABLE git_result
  OUTPUT_VARIABLE production_files)
if(NOT git_result EQUAL 0)
  message(FATAL_ERROR "Cannot enumerate tracked production files")
endif()
file(READ "${SOURCE_DIR}/CMakeLists.txt" root_cmake)
file(GLOB_RECURSE production_dependency_files
     "${BINARY_DIR}/CMakeFiles/minicyber_core.dir/*.d"
     "${BINARY_DIR}/CMakeFiles/minicyber_autodrive_runtime.dir/*.d"
     "${BINARY_DIR}/CMakeFiles/minicyber_autodrive_components.dir/*.d"
     "${BINARY_DIR}/CMakeFiles/minicyber_autodrive_metrics.dir/*.d"
     "${BINARY_DIR}/CMakeFiles/mainboard.dir/*.d"
     "${BINARY_DIR}/CMakeFiles/sensor_source.dir/*.d"
     "${BINARY_DIR}/CMakeFiles/control_sink.dir/*.d")
set(production_dependencies "")
foreach(dependency_file IN LISTS production_dependency_files)
  file(READ "${dependency_file}" dependency_contents)
  string(APPEND production_dependencies "${dependency_contents}\n")
endforeach()
if(production_dependencies STREQUAL "")
  message(FATAL_ERROR "Missing production compiler dependency evidence")
endif()
file(WRITE "${report}" "MC-603_frozen_manifest=docs/refactor/00_进度记录.md\n")
file(APPEND "${report}" "post_MC604_changes=docs/refactor/00_进度记录.md:MC-619\n")
file(APPEND "${report}" "current_git_manifest=git ls-files production roots\n")
file(APPEND "${report}" "runtime_dlopen=libminicyber_autodrive_components.so\n")
string(REPLACE "\n" ";" production_files "${production_files}")
foreach(path IN LISTS production_files)
  if(path STREQUAL "")
    continue()
  endif()
  set(evidence "")
  if(path STREQUAL "CMakeLists.txt")
    set(evidence "explicit_build_definition")
  elseif(path STREQUAL "scripts/run_autodrive_pipeline.sh")
    set(evidence "runtime_launcher")
  elseif(path MATCHES "^config/autodrive/")
    set(evidence "runtime_dag_or_scheduler_input")
  elseif(path MATCHES "^(include|demo/autodrive)/.*\\.h$")
    string(FIND "${production_dependencies}" "/${path}" dependency_position)
    if(NOT dependency_position EQUAL -1)
      set(evidence "production_compile_dependency")
    endif()
  elseif(root_cmake MATCHES "${path}")
    set(evidence "explicit_link_or_compile_chain")
  endif()
  if(evidence STREQUAL "")
    message(FATAL_ERROR "Untouched production file: ${path}")
  endif()
  file(APPEND "${report}" "${path}|${evidence}\n")
endforeach()

find_program(GCOV_EXECUTABLE gcov)
if(GCOV_EXECUTABLE)
  file(GLOB_RECURSE gcda_files "${BINARY_DIR}/*.gcda")
  list(LENGTH gcda_files gcda_count)
  file(APPEND "${report}" "gcov_data_files=${gcda_count}\n")
  set(core_object_dir "${BINARY_DIR}/CMakeFiles/minicyber_core.dir/src/scheduler")
  if(EXISTS "${core_object_dir}/scheduler.cpp.gcda")
    execute_process(
      COMMAND "${GCOV_EXECUTABLE}" -o "${core_object_dir}"
              "${SOURCE_DIR}/src/scheduler/scheduler.cpp"
      WORKING_DIRECTORY "${run_dir}"
      RESULT_VARIABLE gcov_result
      OUTPUT_VARIABLE gcov_output
      ERROR_VARIABLE gcov_error)
    if(NOT gcov_result EQUAL 0)
      message(FATAL_ERROR "gcov failed: ${gcov_output}${gcov_error}")
    endif()
    file(APPEND "${report}" "gcov_scheduler=passed\n")
  endif()
endif()

message("Choreography pipeline evidence: ${run_dir}")
