#include "McpClientManager.hpp"
#include "ToolPolicy.hpp"

#include <QSignalSpy>
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
}  // namespace qtllm::tests

QTEST_GUILESS_MAIN(qtllm::tests::StdioMcpTransportTest)

#include "StdioMcpTransportTest.moc"
