find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol"
    protocol_test_sources
    protocol_test_headers
    protocol_test_forms
)

qt_add_executable(qtllm-protocol-tests
    ${protocol_test_sources}
    ${protocol_test_headers}
    ${protocol_test_forms}
)
target_link_libraries(qtllm-protocol-tests
    PRIVATE
        Qt6::Test
        qtllm_shared
)
add_test(NAME protocol_jsonl COMMAND qtllm-protocol-tests)

qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/logging"
    logging_test_sources
    logging_test_headers
    logging_test_forms
)

qt_add_executable(qtllm-logging-tests
    ${logging_test_sources}
    ${logging_test_headers}
    ${logging_test_forms}
)
target_link_libraries(qtllm-logging-tests
    PRIVATE
        Qt6::Test
        qtllm_shared
)
add_test(NAME logging_file_output COMMAND qtllm-logging-tests)

qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/models"
    model_test_sources
    model_test_headers
    model_test_forms
)

qt_add_executable(qtllm-model-tests
    ${model_test_sources}
    ${model_test_headers}
    ${model_test_forms}
)
target_link_libraries(qtllm-model-tests
    PRIVATE
        Qt6::Test
        qtllm_shared
)
add_test(NAME model_package_validation COMMAND qtllm-model-tests)

qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/agent"
    agent_test_sources
    agent_test_headers
    agent_test_forms
)

qt_add_executable(qtllm-agent-tests
    ${agent_test_sources}
    ${agent_test_headers}
    ${agent_test_forms}
)
target_link_libraries(qtllm-agent-tests
    PRIVATE
        Qt6::Test
        qtllm_shared
)
add_test(NAME agent_action_validation COMMAND qtllm-agent-tests)

qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/ui"
    ui_test_sources
    ui_test_headers
    ui_test_forms
)

qt_add_executable(qtllm-ui-tests
    ${ui_test_sources}
    ${ui_test_headers}
    ${ui_test_forms}
)
target_link_libraries(qtllm-ui-tests
    PRIVATE
        Qt6::Test
        qtllm_app_ui
)
add_test(NAME ui_assistant_response COMMAND qtllm-ui-tests)
set_tests_properties(ui_assistant_response PROPERTIES
    ENVIRONMENT "QTLLM_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-logs"
)

qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/client"
    client_test_sources
    client_test_headers
    client_test_forms
)

qt_add_executable(qtllm-client-tests
    ${client_test_sources}
    ${client_test_headers}
    ${client_test_forms}
)
target_link_libraries(qtllm-client-tests
    PRIVATE
        Qt6::Test
        qtllm_app_infrastructure
)
add_dependencies(qtllm-client-tests qtllm-worker)
add_test(NAME client_worker_lifecycle COMMAND qtllm-client-tests)
set_tests_properties(client_worker_lifecycle PROPERTIES
    ENVIRONMENT "QTLLM_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-logs"
)

qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/application"
    application_test_sources
    application_test_headers
    application_test_forms
)

qt_add_executable(qtllm-application-tests
    ${application_test_sources}
    ${application_test_headers}
    ${application_test_forms}
)
target_link_libraries(qtllm-application-tests
    PRIVATE
        Qt6::Test
        qtllm_app_application
)
add_test(NAME application_chat_controller COMMAND qtllm-application-tests)

qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_controller"
    agent_controller_test_sources
    agent_controller_test_headers
    agent_controller_test_forms
)

qt_add_executable(qtllm-agent-controller-tests
    ${agent_controller_test_sources}
    ${agent_controller_test_headers}
    ${agent_controller_test_forms}
)
target_link_libraries(qtllm-agent-controller-tests
    PRIVATE
        Qt6::Test
        qtllm_app_application
)
add_test(NAME application_agent_controller COMMAND qtllm-agent-controller-tests)

qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/mcp"
    mcp_test_sources
    mcp_test_headers
    mcp_test_forms
)

qt_add_executable(qtllm-mcp-tests
    ${mcp_test_sources}
    ${mcp_test_headers}
    ${mcp_test_forms}
)
target_link_libraries(qtllm-mcp-tests
    PRIVATE
        Qt6::Test
        qtllm_mcp_host
        qtllm_shared
)

qt_add_executable(qtllm-fake-mcp-server
    "${CMAKE_CURRENT_SOURCE_DIR}/fake_mcp_server/FakeMcpServer.cpp"
)
target_link_libraries(qtllm-fake-mcp-server PRIVATE Qt6::Core)

qt_add_executable(qtllm-agent-eval
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_evaluation/AgentEvaluationMain.cpp"
)
target_link_libraries(qtllm-agent-eval
    PRIVATE
        Qt6::Core
        qtllm_app_application
)
add_dependencies(qtllm-agent-eval
    qtllm-worker
    qtllm-fake-mcp-server
)

add_dependencies(qtllm-mcp-tests
    qtllm-fake-mcp-server
    qtllm-mcp-filesystem
)
add_test(NAME mcp_stdio_transport COMMAND qtllm-mcp-tests)
set_tests_properties(mcp_stdio_transport PROPERTIES
    ENVIRONMENT
        "QTLLM_TEST_MCP_SERVER=$<TARGET_FILE:qtllm-fake-mcp-server>;QTLLM_TEST_RUNTIME_DIR=$<TARGET_FILE_DIR:qtllm-mcp-filesystem>"
)

qtllm_collect_module_files(
    "${CMAKE_CURRENT_SOURCE_DIR}/worker"
    worker_test_sources
    worker_test_headers
    worker_test_forms
)

qt_add_executable(qtllm-worker-tests
    ${worker_test_sources}
    ${worker_test_headers}
    ${worker_test_forms}
)
target_link_libraries(qtllm-worker-tests
    PRIVATE
        Qt6::Test
        qtllm_shared
)
add_dependencies(qtllm-worker-tests qtllm-worker)
add_test(NAME worker_jsonl_ipc COMMAND qtllm-worker-tests)
set_tests_properties(worker_jsonl_ipc PROPERTIES
    ENVIRONMENT
        "QTLLM_TEST_WORKER=$<TARGET_FILE:qtllm-worker>;QTLLM_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-logs"
)

add_test(NAME worker_help COMMAND qtllm-worker --help)
add_test(NAME worker_version COMMAND qtllm-worker --version)
add_test(NAME worker_requires_model
    COMMAND ${CMAKE_COMMAND}
        -DWORKER=$<TARGET_FILE:qtllm-worker>
        -DCASE=requires_model
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AssertWorkerFailure.cmake
)

add_test(NAME worker_rejects_invalid_number
    COMMAND ${CMAKE_COMMAND}
        -DWORKER=$<TARGET_FILE:qtllm-worker>
        -DCASE=invalid_context_size
        -DMODEL=${CMAKE_SOURCE_DIR}/CMakeLists.txt
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AssertWorkerFailure.cmake
)

add_test(NAME worker_rejects_invalid_model
    COMMAND ${CMAKE_COMMAND}
        -DWORKER=$<TARGET_FILE:qtllm-worker>
        -DCASE=invalid_model
        -DMODEL=${CMAKE_SOURCE_DIR}/CMakeLists.txt
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AssertWorkerFailure.cmake
)

set_tests_properties(
    worker_help
    worker_version
    worker_requires_model
    worker_rejects_invalid_number
    worker_rejects_invalid_model
    PROPERTIES
        ENVIRONMENT "QTLLM_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-logs"
)
