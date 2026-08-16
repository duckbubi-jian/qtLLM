#include "BuiltInMcpServer.hpp"
#include "McpClientManager.hpp"
#include "ToolPolicy.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace qtllm::tests
{
class StdioMcpTransportTest final : public QObject
{
    Q_OBJECT

   private slots:
    void listsAndCallsTools();
    void propagatesRemoteErrors();
    void timesOutAndCanCancel();
    void validatesArgumentsBeforeCallingServer();
    void appliesLocalToolPolicy();
    void roundTripsServerConfiguration();
    void rejectsInvalidServerIds();
    void builtInFilesystemWritesCppFile();
};

infrastructure::mcp::McpServerConfig testConfig()
{
    infrastructure::mcp::McpServerConfig config;
    config.serverId = QStringLiteral("fake");
    config.program = qEnvironmentVariable("QTLLM_TEST_MCP_SERVER");
    config.initializeTimeoutMs = 2'000;
    config.requestTimeoutMs = 2'000;
    return config;
}

void StdioMcpTransportTest::listsAndCallsTools()
{
    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY2(manager.addServer(testConfig(), errorMessage),
             qPrintable(errorMessage));
    QSignalSpy startedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverStarted);
    manager.startServer(QStringLiteral("fake"));
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 2'000);

    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    QVERIFY(!manager.initialize(QStringLiteral("fake")).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);

    QSignalSpy toolsSpy(&manager,
                        &infrastructure::mcp::McpClientManager::toolsChanged);
    QVERIFY(!manager.listTools(QStringLiteral("fake")).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(toolsSpy.count(), 1, 2'000);
    const auto tools = manager.tools();
    QCOMPARE(tools.size(), 3);
    QVERIFY(manager.registry().find(QStringLiteral("fake.echo")) != nullptr);

    QSignalSpy resultSpy(
        &manager, &infrastructure::mcp::McpClientManager::toolResultReady);
    const auto requestId =
        manager.callTool(QStringLiteral("fake.echo"),
                         {{QStringLiteral("value"), QStringLiteral("hello")}});
    QVERIFY(!requestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 1, 2'000);
    const auto result = qvariant_cast<agent::ToolResult>(resultSpy.at(0).at(0));
    QCOMPARE(result.requestId, requestId);
    QCOMPARE(result.serverId, QStringLiteral("fake"));
    QVERIFY(!result.isError);

    manager.stopServer(QStringLiteral("fake"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !manager.transport(QStringLiteral("fake"))->isRunning(), 2'000);
}

void StdioMcpTransportTest::propagatesRemoteErrors()
{
    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    auto config = testConfig();
    config.requestTimeoutMs = 2'000;
    QVERIFY(manager.addServer(config, errorMessage));
    manager.startServer(QStringLiteral("fake"));
    QTRY_VERIFY_WITH_TIMEOUT(
        manager.transport(QStringLiteral("fake"))->isRunning(), 2'000);
    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    manager.initialize(QStringLiteral("fake"));
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);
    manager.listTools(QStringLiteral("fake"));
    QTRY_VERIFY_WITH_TIMEOUT(!manager.tools().isEmpty(), 2'000);

    QSignalSpy resultSpy(
        &manager, &infrastructure::mcp::McpClientManager::toolResultReady);
    QVERIFY(!manager.callTool(QStringLiteral("fake.error"), {}).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 1, 2'000);
    QVERIFY(qvariant_cast<agent::ToolResult>(resultSpy.at(0).at(0)).isError);
}

void StdioMcpTransportTest::timesOutAndCanCancel()
{
    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    auto config = testConfig();
    config.requestTimeoutMs = 1'000;
    QVERIFY(manager.addServer(config, errorMessage));
    manager.startServer(QStringLiteral("fake"));
    QTRY_VERIFY_WITH_TIMEOUT(
        manager.transport(QStringLiteral("fake"))->isRunning(), 2'000);
    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    manager.initialize(QStringLiteral("fake"));
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);
    manager.listTools(QStringLiteral("fake"));
    QTRY_VERIFY_WITH_TIMEOUT(!manager.tools().isEmpty(), 2'000);

    QSignalSpy failureSpy(
        &manager, &infrastructure::mcp::McpClientManager::requestFailed);
    const auto requestId = manager.callTool(QStringLiteral("fake.slow"), {});
    QVERIFY(!requestId.isEmpty());
    manager.cancel(requestId);
    QTRY_COMPARE_WITH_TIMEOUT(failureSpy.count(), 1, 1'000);
    QCOMPARE(failureSpy.at(0).at(3).toString(), QStringLiteral("cancelled"));

    const auto timeoutRequestId =
        manager.callTool(QStringLiteral("fake.slow"), {});
    QVERIFY(!timeoutRequestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(failureSpy.count(), 2, 3'000);
    QCOMPARE(failureSpy.at(1).at(3).toString(), QStringLiteral("timeout"));
}

void StdioMcpTransportTest::validatesArgumentsBeforeCallingServer()
{
    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY(manager.addServer(testConfig(), errorMessage));
    manager.startServer(QStringLiteral("fake"));
    QTRY_VERIFY_WITH_TIMEOUT(
        manager.transport(QStringLiteral("fake"))->isRunning(), 2'000);
    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    manager.initialize(QStringLiteral("fake"));
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);
    manager.listTools(QStringLiteral("fake"));
    QTRY_VERIFY_WITH_TIMEOUT(!manager.tools().isEmpty(), 2'000);

    QSignalSpy failureSpy(
        &manager, &infrastructure::mcp::McpClientManager::requestFailed);
    QVERIFY(manager.callTool(QStringLiteral("fake.echo"), {}).isEmpty());
    QCOMPARE(failureSpy.count(), 1);
    QVERIFY(failureSpy.at(0).at(4).toString().contains(
        QStringLiteral("arguments.value is required")));
    QVERIFY(manager
                .callTool(QStringLiteral("fake.echo"),
                          {{QStringLiteral("value"), 42}})
                .isEmpty());
    QCOMPARE(failureSpy.count(), 2);
    QVERIFY(!manager
                 .callTool(QStringLiteral("fake.echo"),
                           {{QStringLiteral("value"), QStringLiteral("valid")}})
                 .isEmpty());
}

void StdioMcpTransportTest::appliesLocalToolPolicy()
{
    infrastructure::mcp::ToolPolicy policy;
    QCOMPARE(policy.evaluate(QStringLiteral("fake.echo")),
             infrastructure::mcp::ToolDecision::RequireApproval);
    policy.setRule(QStringLiteral("fake.echo"),
                   {infrastructure::mcp::ToolRisk::ReadOnly, true, true});
    QCOMPARE(policy.evaluate(QStringLiteral("fake.echo")),
             infrastructure::mcp::ToolDecision::Allow);
    policy.setRule(QStringLiteral("fake.echo"),
                   {infrastructure::mcp::ToolRisk::Destructive, false, false});
    QCOMPARE(policy.evaluate(QStringLiteral("fake.echo")),
             infrastructure::mcp::ToolDecision::Deny);

    const auto readRule = infrastructure::mcp::defaultToolPolicyRule(
        QStringLiteral("filesystem.read_text_file"));
    QCOMPARE(readRule.risk, infrastructure::mcp::ToolRisk::ReadOnly);
    QVERIFY(readRule.alwaysAllow);
    const auto writeRule = infrastructure::mcp::defaultToolPolicyRule(
        QStringLiteral("filesystem.write_file"));
    QCOMPARE(writeRule.risk, infrastructure::mcp::ToolRisk::ModifiesData);
    QVERIFY(!writeRule.alwaysAllow);
    const auto unknownRule = infrastructure::mcp::defaultToolPolicyRule(
        QStringLiteral("thirdparty.read_text_file"));
    QCOMPARE(unknownRule.risk, infrastructure::mcp::ToolRisk::ModifiesData);
    QVERIFY(!unknownRule.alwaysAllow);
}

void StdioMcpTransportTest::roundTripsServerConfiguration()
{
    auto config = testConfig();
    config.arguments = {QStringLiteral("--flag"), QStringLiteral("value")};
    config.environment.insert(QStringLiteral("TOKEN"),
                              QStringLiteral("not-a-real-secret"));
    config.workingDirectory = QStringLiteral("D:/workspace");
    config.toolAllowlist = {QStringLiteral("echo")};
    config.maxResultBytes = 8'192;

    infrastructure::mcp::McpServerConfig parsed;
    QString errorMessage;
    QVERIFY2(infrastructure::mcp::parseServerConfig(
                 infrastructure::mcp::serializeServerConfig(config), parsed,
                 errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(parsed.serverId, config.serverId);
    QCOMPARE(parsed.program, config.program);
    QCOMPARE(parsed.arguments, config.arguments);
    QCOMPARE(parsed.environment, config.environment);
    QCOMPARE(parsed.toolAllowlist, config.toolAllowlist);
    QCOMPARE(parsed.maxResultBytes, config.maxResultBytes);
}

void StdioMcpTransportTest::rejectsInvalidServerIds()
{
    auto config = testConfig();
    config.serverId = QStringLiteral("invalid.server");
    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY(!manager.addServer(config, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("serverId")));
    QVERIFY(manager.serverIds().isEmpty());
}

void StdioMcpTransportTest::builtInFilesystemWritesCppFile()
{
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const auto workspaceRoot =
        QDir(workspace.path()).filePath(QStringLiteral("工作区"));
    QVERIFY(QDir().mkpath(workspaceRoot));
    const auto runtimeDirectory =
        qEnvironmentVariable("QTLLM_TEST_RUNTIME_DIR");
    QVERIFY2(!runtimeDirectory.isEmpty(),
             "QTLLM_TEST_RUNTIME_DIR is not configured");

    infrastructure::mcp::McpServerConfig config;
    QString errorMessage;
    QVERIFY2(infrastructure::mcp::createBuiltInFilesystemServerConfig(
                 runtimeDirectory, QStringLiteral("0.2.0"), workspaceRoot,
                 config, errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(config.serverId, QStringLiteral("filesystem"));
    QCOMPARE(config.arguments,
             QStringList({QStringLiteral("--write-root"), workspaceRoot}));

    infrastructure::mcp::McpClientManager manager;
    QVERIFY2(manager.addServer(config, errorMessage), qPrintable(errorMessage));
    QSignalSpy startedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverStarted);
    manager.startServer(config.serverId);
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 2'000);

    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    QVERIFY(!manager.initialize(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);

    QSignalSpy toolsSpy(&manager,
                        &infrastructure::mcp::McpClientManager::toolsChanged);
    QVERIFY(!manager.listTools(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(toolsSpy.count(), 1, 2'000);
    QVERIFY(manager.registry().find(QStringLiteral("filesystem.write_file")) !=
            nullptr);

    const auto cppPath =
        QDir(workspaceRoot).filePath(QStringLiteral("示例.cpp"));
    const auto cppSource = QStringLiteral(
        "#include <iostream>\n\nint main()\n{\n    std::cout << "
        "\"hello\" << '\\n';\n    return 0;\n}\n");
    QSignalSpy resultSpy(
        &manager, &infrastructure::mcp::McpClientManager::toolResultReady);
    const auto createRequestId = manager.callTool(
        QStringLiteral("filesystem.create_directory"),
        {{QStringLiteral("path"), QStringLiteral("./generated")}});
    QVERIFY(!createRequestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 1, 2'000);
    const auto createResult =
        qvariant_cast<agent::ToolResult>(resultSpy.at(0).at(0));
    QVERIFY2(!createResult.isError, qPrintable(createResult.errorMessage));
    QVERIFY(QFileInfo(QDir(workspaceRoot).filePath(QStringLiteral("generated")))
                .isDir());

    const auto relativeCppPath =
        QStringLiteral("./") + QFileInfo(cppPath).fileName();
    const auto requestId =
        manager.callTool(QStringLiteral("filesystem.write_file"),
                         {{QStringLiteral("path"), relativeCppPath},
                          {QStringLiteral("content"), cppSource}});
    QVERIFY(!requestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 2, 2'000);
    const auto result = qvariant_cast<agent::ToolResult>(resultSpy.at(1).at(0));
    QVERIFY2(!result.isError, qPrintable(result.errorMessage));

    QFile cppFile(cppPath);
    QVERIFY(cppFile.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(cppFile.readAll()), cppSource);

    const auto escapedPath =
        QDir(workspace.path()).filePath(QStringLiteral("escaped.cpp"));
    const auto escapedRequestId = manager.callTool(
        QStringLiteral("filesystem.write_file"),
        {{QStringLiteral("path"), QStringLiteral("../escaped.cpp")},
         {QStringLiteral("content"), cppSource}});
    QVERIFY(!escapedRequestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 3, 2'000);
    const auto escapedResult =
        qvariant_cast<agent::ToolResult>(resultSpy.at(2).at(0));
    QVERIFY(escapedResult.isError);
    QVERIFY(!QFileInfo::exists(escapedPath));

    QTemporaryDir outsideWorkspace;
    QVERIFY(outsideWorkspace.isValid());
    const auto outsidePath =
        QDir(outsideWorkspace.path()).filePath(QStringLiteral("outside.cpp"));
    const auto rejectedRequestId =
        manager.callTool(QStringLiteral("filesystem.write_file"),
                         {{QStringLiteral("path"), outsidePath},
                          {QStringLiteral("content"), cppSource}});
    QVERIFY(!rejectedRequestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 4, 2'000);
    const auto rejectedResult =
        qvariant_cast<agent::ToolResult>(resultSpy.at(3).at(0));
    QVERIFY(rejectedResult.isError);
    QVERIFY(!QFileInfo::exists(outsidePath));
}
}  // namespace qtllm::tests

QTEST_GUILESS_MAIN(qtllm::tests::StdioMcpTransportTest)

#include "StdioMcpTransportTest.moc"
