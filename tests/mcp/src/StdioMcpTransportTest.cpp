#include "BuiltInMcpServer.hpp"
#include "McpClientManager.hpp"
#include "McpHostRuntime.hpp"
#include "McpProtocol.hpp"
#include "McpServerRegistry.hpp"
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
namespace
{
class FakeMcpTransport final : public infrastructure::mcp::McpTransport
{
   public:
    using McpTransport::McpTransport;

    [[nodiscard]] bool isRunning() const override
    {
        return running_;
    }

    void start() override
    {
        running_ = true;
        emit started();
    }

    void stop() override
    {
        if (!running_) return;
        running_ = false;
        emit stopped();
    }

    QString request(const QString& method, const QJsonObject& params,
                    int) override
    {
        if (!running_) return {};
        const auto requestId = QString::number(++nextRequestId_);
        requests_.insert(requestId, method);
        requestParams_.insert(requestId, params);
        return requestId;
    }

    void cancel(const QString& requestId) override
    {
        const auto method = requests_.take(requestId);
        requestParams_.remove(requestId);
        if (!method.isEmpty())
            emit requestFailed(requestId, method, QStringLiteral("cancelled"),
                               QStringLiteral("Request cancelled."));
    }

    void cancelAll() override
    {
        const auto requestIds = requests_.keys();
        for (const auto& requestId : requestIds)
            cancel(requestId);
    }

    bool notify(const QString& method, const QJsonObject&) override
    {
        notifications_.append(method);
        return running_;
    }

    void respond(const QString& requestId, const QJsonObject& result)
    {
        const auto method = requests_.take(requestId);
        requestParams_.remove(requestId);
        if (!method.isEmpty()) emit responseReceived(requestId, method, result);
    }

    void fail(const QString& requestId, const QString& code,
              const QString& message)
    {
        const auto method = requests_.take(requestId);
        requestParams_.remove(requestId);
        if (!method.isEmpty())
            emit requestFailed(requestId, method, code, message);
    }

    [[nodiscard]] QString requestForMethod(const QString& method) const
    {
        for (auto iterator = requests_.constBegin();
             iterator != requests_.constEnd(); ++iterator)
            if (iterator.value() == method) return iterator.key();
        return {};
    }

    [[nodiscard]] QJsonObject requestParams(const QString& requestId) const
    {
        return requestParams_.value(requestId);
    }

    void sendNotification(const QString& method, const QJsonObject& params)
    {
        emit notificationReceived(method, params);
    }

    void crash()
    {
        running_ = false;
        emit transportError(QStringLiteral("crashed"),
                            QStringLiteral("Fake transport crashed."));
        emit stopped();
    }

   private:
    bool running_ = false;
    int nextRequestId_ = 0;
    QHash<QString, QString> requests_;
    QHash<QString, QJsonObject> requestParams_;
    QStringList notifications_;
};

infrastructure::mcp::McpServerConfig runtimeTestConfig(const QString& serverId)
{
    infrastructure::mcp::McpServerConfig config;
    config.serverId = serverId;
    config.program = QDir::root().filePath(QStringLiteral("fake-mcp-server"));
    return config;
}

QJsonObject initializeResult(bool toolsListChanged = false)
{
    QJsonObject toolsCapability;
    if (toolsListChanged)
        toolsCapability.insert(QStringLiteral("listChanged"), true);
    return {{QStringLiteral("protocolVersion"),
             infrastructure::mcp::latestSupportedProtocolVersion()},
            {QStringLiteral("capabilities"),
             QJsonObject{{QStringLiteral("tools"), toolsCapability}}},
            {QStringLiteral("serverInfo"),
             QJsonObject{{QStringLiteral("name"), QStringLiteral("fake")},
                         {QStringLiteral("version"), QStringLiteral("1")}}}};
}

QJsonObject toolsResult(const QString& name = QStringLiteral("echo"))
{
    return {{QStringLiteral("tools"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("name"), name},
                 {QStringLiteral("description"), QStringLiteral("Echo")},
                 {QStringLiteral("inputSchema"),
                  QJsonObject{
                      {QStringLiteral("type"), QStringLiteral("object")}}}}}}};
}
}  // namespace

class StdioMcpTransportTest final : public QObject
{
    Q_OBJECT

   private slots:
    void listsAndCallsTools();
    void parsesInitializeCapabilities();
    void rejectsUnsupportedProtocolVersion();
    void rejectsUndeclaredToolsCapability();
    void propagatesRemoteErrors();
    void timesOutAndCanCancel();
    void validatesArgumentsBeforeCallingServer();
    void appliesLocalToolPolicy();
    void roundTripsServerConfiguration();
    void rejectsInvalidServerIds();
    void builtInFilesystemWritesCppFile();
    void tracksServerLifecycleAndRevokesCapabilities();
    void isolatesMultipleServerFailures();
    void rejectsInvalidServerStateTransitions();
    void paginatesToolsAtomicallyAndPreservesMetadata();
    void refreshesToolsFromListChangedNotification();
    void routesTypedNotificationsAndRichResults();
    void limitsNotificationStorms();
    void sendsProtocolCancellation();
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

void StdioMcpTransportTest::parsesInitializeCapabilities()
{
    QCOMPARE(infrastructure::mcp::latestSupportedProtocolVersion(),
             QStringLiteral("2024-11-05"));
    QVERIFY(infrastructure::mcp::isSupportedProtocolVersion(
        QStringLiteral("2024-11-05")));
    QVERIFY(!infrastructure::mcp::isSupportedProtocolVersion(
        QStringLiteral("2099-01-01")));
    QVERIFY(!infrastructure::mcp::isSupportedProtocolVersion(
        QStringLiteral(" 2024-11-05")));

    const QJsonObject capabilities{
        {QStringLiteral("tools"),
         QJsonObject{{QStringLiteral("listChanged"), true}}},
        {QStringLiteral("resources"),
         QJsonObject{{QStringLiteral("subscribe"), true},
                     {QStringLiteral("listChanged"), true}}},
        {QStringLiteral("prompts"),
         QJsonObject{{QStringLiteral("listChanged"), true}}},
        {QStringLiteral("logging"), QJsonObject{}},
        {QStringLiteral("completions"), QJsonObject{}}};
    const QJsonObject initialize{
        {QStringLiteral("protocolVersion"), QStringLiteral("2024-11-05")},
        {QStringLiteral("capabilities"), capabilities},
        {QStringLiteral("serverInfo"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("test")},
                     {QStringLiteral("version"), QStringLiteral("1")}}},
        {QStringLiteral("instructions"), QStringLiteral("Test guidance")}};

    infrastructure::mcp::McpInitializeResult result;
    QString errorMessage;
    QVERIFY2(infrastructure::mcp::parseInitializeResult(initialize, result,
                                                        errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(result.protocolVersion, QStringLiteral("2024-11-05"));
    QVERIFY(result.capabilities.tools);
    QVERIFY(result.capabilities.toolsListChanged);
    QVERIFY(result.capabilities.resources);
    QVERIFY(result.capabilities.resourcesSubscribe);
    QVERIFY(result.capabilities.resourcesListChanged);
    QVERIFY(result.capabilities.prompts);
    QVERIFY(result.capabilities.promptsListChanged);
    QVERIFY(result.capabilities.logging);
    QVERIFY(result.capabilities.completions);
    QCOMPARE(result.capabilities.raw, capabilities);
    QCOMPARE(result.instructions, QStringLiteral("Test guidance"));

    auto invalid = initialize;
    invalid.insert(QStringLiteral("capabilities"),
                   QJsonObject{{QStringLiteral("tools"), true}});
    QVERIFY(!infrastructure::mcp::parseInitializeResult(invalid, result,
                                                        errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("tools")));
}

void StdioMcpTransportTest::rejectsUnsupportedProtocolVersion()
{
    auto config = testConfig();
    config.arguments.append(QStringLiteral("--protocol-version=2099-01-01"));
    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY2(manager.addServer(config, errorMessage), qPrintable(errorMessage));
    QSignalSpy startedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverStarted);
    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    QSignalSpy failureSpy(
        &manager, &infrastructure::mcp::McpClientManager::requestFailed);

    manager.startServer(config.serverId);
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 2'000);
    QVERIFY(!manager.initialize(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(failureSpy.count(), 1, 2'000);
    QCOMPARE(initializedSpy.count(), 0);
    QCOMPARE(failureSpy.at(0).at(2).toString(), QStringLiteral("initialize"));
    QCOMPARE(failureSpy.at(0).at(3).toString(),
             QStringLiteral("invalid_response"));
    QVERIFY(failureSpy.at(0).at(4).toString().contains(
        QStringLiteral("Unsupported MCP protocol version")));
    QVERIFY(manager.agentInstructions().isEmpty());
}

void StdioMcpTransportTest::rejectsUndeclaredToolsCapability()
{
    auto config = testConfig();
    config.arguments.append(QStringLiteral("--no-tools-capability"));
    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY2(manager.addServer(config, errorMessage), qPrintable(errorMessage));
    QSignalSpy startedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverStarted);
    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    QSignalSpy failureSpy(
        &manager, &infrastructure::mcp::McpClientManager::requestFailed);

    manager.startServer(config.serverId);
    QTRY_COMPARE_WITH_TIMEOUT(startedSpy.count(), 1, 2'000);
    QVERIFY(!manager.initialize(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);
    QVERIFY(manager.listTools(config.serverId).isEmpty());
    QCOMPARE(failureSpy.count(), 1);
    QCOMPARE(failureSpy.at(0).at(2).toString(), QStringLiteral("tools/list"));
    QCOMPARE(failureSpy.at(0).at(3).toString(),
             QStringLiteral("invalid_request"));
    QVERIFY(failureSpy.at(0).at(4).toString().contains(
        QStringLiteral("did not declare the Tools capability")));
    QVERIFY(manager.tools().isEmpty());
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
    QCOMPARE(manager.agentInstructions(),
             QStringLiteral("fake: Use fake tools carefully."));

    QSignalSpy toolsSpy(&manager,
                        &infrastructure::mcp::McpClientManager::toolsChanged);
    QVERIFY(!manager.listTools(QStringLiteral("fake")).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(toolsSpy.count(), 1, 2'000);
    const auto tools = manager.tools();
    QCOMPARE(tools.size(), 4);
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
    const auto remoteError =
        qvariant_cast<agent::ToolResult>(resultSpy.at(0).at(0));
    QVERIFY(remoteError.isError);
    QCOMPARE(remoteError.failureKind, agent::ToolFailureKind::Server);

    QVERIFY(
        !manager.callTool(QStringLiteral("fake.business_error"), {}).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 2, 2'000);
    const auto businessError =
        qvariant_cast<agent::ToolResult>(resultSpy.at(1).at(0));
    QVERIFY(businessError.isError);
    QCOMPARE(businessError.failureKind, agent::ToolFailureKind::Tool);
    QCOMPARE(businessError.errorCode, QStringLiteral("CASE_PATH_EXISTS"));
    QCOMPARE(businessError.errorMessage,
             QStringLiteral("Case path already exists."));
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

void StdioMcpTransportTest::tracksServerLifecycleAndRevokesCapabilities()
{
    using infrastructure::mcp::McpHostRuntime;
    using infrastructure::mcp::McpServerState;

    QHash<QString, QSharedPointer<FakeMcpTransport>> transports;
    McpHostRuntime runtime(
        [&transports](const infrastructure::mcp::McpServerConfig& config)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            auto transport = QSharedPointer<FakeMcpTransport>::create();
            transports.insert(config.serverId, transport);
            return transport;
        });
    QString errorMessage;
    QVERIFY2(runtime.addServer(runtimeTestConfig(QStringLiteral("alpha")),
                               errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->state,
             McpServerState::Stopped);

    QSignalSpy stateSpy(&runtime, &McpHostRuntime::serverStateChanged);
    runtime.startServer(QStringLiteral("alpha"));
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->state,
             McpServerState::Starting);
    const auto initializeRequest = runtime.initialize(QStringLiteral("alpha"));
    QVERIFY(!initializeRequest.isEmpty());
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->state,
             McpServerState::Initializing);
    transports.value(QStringLiteral("alpha"))
        ->respond(initializeRequest, initializeResult());
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->state,
             McpServerState::Ready);

    const auto listRequest = runtime.listTools(QStringLiteral("alpha"));
    QVERIFY(!listRequest.isEmpty());
    transports.value(QStringLiteral("alpha"))
        ->respond(listRequest, toolsResult());
    QCOMPARE(runtime.tools().size(), 1);
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->toolCount, 1);

    const auto failedRefresh = runtime.listTools(QStringLiteral("alpha"));
    transports.value(QStringLiteral("alpha"))
        ->fail(failedRefresh, QStringLiteral("refresh_failed"),
               QStringLiteral("Refresh failed."));
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->state,
             McpServerState::Degraded);
    QCOMPARE(runtime.tools().size(), 1);

    runtime.stopServer(QStringLiteral("alpha"));
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->state,
             McpServerState::Stopped);
    QVERIFY(runtime.tools().isEmpty());
    const auto stoppedSnapshot =
        runtime.serverSnapshot(QStringLiteral("alpha")).value();
    QVERIFY(stoppedSnapshot.protocolVersion.isEmpty());
    QVERIFY(!stoppedSnapshot.capabilities.tools);
    QCOMPARE(stoppedSnapshot.toolCount, 0);

    runtime.startServer(QStringLiteral("alpha"));
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->state,
             McpServerState::Starting);
    QVERIFY(stateSpy.count() >= 7);
}

void StdioMcpTransportTest::isolatesMultipleServerFailures()
{
    using infrastructure::mcp::McpHostRuntime;
    using infrastructure::mcp::McpServerState;

    QHash<QString, QSharedPointer<FakeMcpTransport>> transports;
    McpHostRuntime runtime(
        [&transports](const infrastructure::mcp::McpServerConfig& config)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            auto transport = QSharedPointer<FakeMcpTransport>::create();
            transports.insert(config.serverId, transport);
            return transport;
        });
    QString errorMessage;
    for (const auto& serverId :
         {QStringLiteral("alpha"), QStringLiteral("beta")})
    {
        QVERIFY2(runtime.addServer(runtimeTestConfig(serverId), errorMessage),
                 qPrintable(errorMessage));
        runtime.startServer(serverId);
        const auto initializeRequest = runtime.initialize(serverId);
        transports.value(serverId)->respond(initializeRequest,
                                            initializeResult());
        const auto listRequest = runtime.listTools(serverId);
        transports.value(serverId)->respond(listRequest, toolsResult());
    }
    QCOMPARE(runtime.tools().size(), 2);
    QVERIFY(runtime.registry().find(QStringLiteral("alpha.echo")) != nullptr);
    QVERIFY(runtime.registry().find(QStringLiteral("beta.echo")) != nullptr);

    transports.value(QStringLiteral("alpha"))->crash();
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->state,
             McpServerState::Failed);
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("beta"))->state,
             McpServerState::Ready);
    QVERIFY(runtime.registry().find(QStringLiteral("alpha.echo")) == nullptr);
    QVERIFY(runtime.registry().find(QStringLiteral("beta.echo")) != nullptr);

    QSignalSpy resultSpy(&runtime, &McpHostRuntime::toolResultReady);
    const auto callRequest =
        runtime.callTool(QStringLiteral("beta.echo"), QJsonObject{});
    QVERIFY(!callRequest.isEmpty());
    transports.value(QStringLiteral("beta"))
        ->respond(callRequest,
                  {{QStringLiteral("content"),
                    QJsonArray{QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("ok")}}}}});
    QCOMPARE(resultSpy.count(), 1);
    const auto result =
        qvariant_cast<agent::ToolResult>(resultSpy.constFirst().constFirst());
    QCOMPARE(result.serverId, QStringLiteral("beta"));
    QVERIFY(!result.isError);
}

void StdioMcpTransportTest::rejectsInvalidServerStateTransitions()
{
    infrastructure::mcp::McpServerRegistry registry;
    QString errorMessage;
    QVERIFY(registry.addServer(QStringLiteral("alpha"), errorMessage));
    QVERIFY(!registry.transition(QStringLiteral("alpha"),
                                 infrastructure::mcp::McpServerState::Ready,
                                 errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("stopped -> ready")));
    QCOMPARE(registry.snapshot(QStringLiteral("alpha"))->state,
             infrastructure::mcp::McpServerState::Stopped);
}

void StdioMcpTransportTest::paginatesToolsAtomicallyAndPreservesMetadata()
{
    using infrastructure::mcp::McpHostRuntime;
    using infrastructure::mcp::McpServerState;

    QHash<QString, QSharedPointer<FakeMcpTransport>> transports;
    McpHostRuntime runtime(
        [&transports](const infrastructure::mcp::McpServerConfig& config)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            auto transport = QSharedPointer<FakeMcpTransport>::create();
            transports.insert(config.serverId, transport);
            return transport;
        });
    QString errorMessage;
    QVERIFY(runtime.addServer(runtimeTestConfig(QStringLiteral("alpha")),
                              errorMessage));
    runtime.startServer(QStringLiteral("alpha"));
    auto* transport = transports.value(QStringLiteral("alpha")).get();
    const auto initializeRequest = runtime.initialize(QStringLiteral("alpha"));
    transport->respond(initializeRequest, initializeResult());

    auto requestId = runtime.listTools(QStringLiteral("alpha"));
    transport->respond(requestId, toolsResult(QStringLiteral("old")));
    QCOMPARE(runtime.tools().size(), 1);
    QCOMPARE(runtime.tools().constFirst().name, QStringLiteral("old"));
    const auto initialRevision =
        runtime.serverSnapshot(QStringLiteral("alpha"))->capabilityRevision;
    QSignalSpy toolsSpy(&runtime, &McpHostRuntime::toolsChanged);

    requestId = runtime.listTools(QStringLiteral("alpha"));
    transport->respond(
        requestId,
        {{QStringLiteral("tools"),
          QJsonArray{QJsonObject{
              {QStringLiteral("name"), QStringLiteral("first")},
              {QStringLiteral("description"), QStringLiteral("First")},
              {QStringLiteral("inputSchema"),
               QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}},
              {QStringLiteral("outputSchema"),
               QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}}},
              {QStringLiteral("annotations"),
               QJsonObject{{QStringLiteral("readOnlyHint"), true}}}}}},
         {QStringLiteral("nextCursor"), QStringLiteral("page-2")}});
    QCOMPARE(runtime.tools().size(), 1);
    QCOMPARE(runtime.tools().constFirst().name, QStringLiteral("old"));
    QCOMPARE(toolsSpy.count(), 0);
    QCOMPARE(
        runtime.serverSnapshot(QStringLiteral("alpha"))->capabilityRevision,
        initialRevision);

    const auto secondPageRequest =
        transport->requestForMethod(QStringLiteral("tools/list"));
    QVERIFY(!secondPageRequest.isEmpty());
    QCOMPARE(transport->requestParams(secondPageRequest)
                 .value(QStringLiteral("cursor"))
                 .toString(),
             QStringLiteral("page-2"));
    transport->respond(
        secondPageRequest,
        {{QStringLiteral("tools"),
          QJsonArray{
              QJsonObject{{QStringLiteral("name"), QStringLiteral("second")},
                          {QStringLiteral("inputSchema"), QJsonObject{}},
                          {QStringLiteral("outputSchema"), QJsonObject{}}}}}});
    QCOMPARE(toolsSpy.count(), 1);
    QCOMPARE(runtime.tools().size(), 2);
    const auto* first = runtime.registry().find(QStringLiteral("alpha.first"));
    QVERIFY(first != nullptr);
    QVERIFY(first->hasOutputSchema);
    QCOMPARE(first->outputSchema.value(QStringLiteral("type")).toString(),
             QStringLiteral("array"));
    QVERIFY(first->annotations.value(QStringLiteral("readOnlyHint")).toBool());
    const auto* second =
        runtime.registry().find(QStringLiteral("alpha.second"));
    QVERIFY(second != nullptr);
    QVERIFY(second->hasOutputSchema);
    QVERIFY(second->outputSchema.isEmpty());
    QVERIFY(
        runtime.serverSnapshot(QStringLiteral("alpha"))->capabilityRevision >
        initialRevision);

    requestId = runtime.listTools(QStringLiteral("alpha"));
    transport->respond(
        requestId,
        {{QStringLiteral("tools"),
          QJsonArray{QJsonObject{
              {QStringLiteral("name"), QStringLiteral("replacement")},
              {QStringLiteral("inputSchema"), QJsonObject{}}}}},
         {QStringLiteral("nextCursor"), QStringLiteral("fails")}});
    const auto failedPageRequest =
        transport->requestForMethod(QStringLiteral("tools/list"));
    transport->fail(failedPageRequest, QStringLiteral("page_failed"),
                    QStringLiteral("Second page failed."));
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->state,
             McpServerState::Degraded);
    QCOMPARE(runtime.tools().size(), 2);
    QVERIFY(runtime.registry().find(QStringLiteral("alpha.first")) != nullptr);
    QVERIFY(runtime.registry().find(QStringLiteral("alpha.replacement")) ==
            nullptr);
}

void StdioMcpTransportTest::refreshesToolsFromListChangedNotification()
{
    using infrastructure::mcp::McpHostRuntime;

    QHash<QString, QSharedPointer<FakeMcpTransport>> transports;
    McpHostRuntime runtime(
        [&transports](const infrastructure::mcp::McpServerConfig& config)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            auto transport = QSharedPointer<FakeMcpTransport>::create();
            transports.insert(config.serverId, transport);
            return transport;
        });
    QString errorMessage;
    QVERIFY(runtime.addServer(runtimeTestConfig(QStringLiteral("alpha")),
                              errorMessage));
    runtime.startServer(QStringLiteral("alpha"));
    auto* transport = transports.value(QStringLiteral("alpha")).get();
    const auto initializeRequest = runtime.initialize(QStringLiteral("alpha"));
    transport->respond(initializeRequest, initializeResult(true));
    auto requestId = runtime.listTools(QStringLiteral("alpha"));
    transport->respond(requestId, toolsResult(QStringLiteral("old")));

    transport->sendNotification(
        QStringLiteral("notifications/tools/list_changed"), {});
    requestId = transport->requestForMethod(QStringLiteral("tools/list"));
    QVERIFY(!requestId.isEmpty());
    transport->sendNotification(
        QStringLiteral("notifications/tools/list_changed"), {});
    transport->sendNotification(
        QStringLiteral("notifications/tools/list_changed"), {});
    transport->respond(requestId, toolsResult(QStringLiteral("new")));

    const auto coalescedRequest =
        transport->requestForMethod(QStringLiteral("tools/list"));
    QVERIFY(!coalescedRequest.isEmpty());
    transport->respond(coalescedRequest, toolsResult(QStringLiteral("newest")));
    QCOMPARE(runtime.tools().size(), 1);
    QCOMPARE(runtime.tools().constFirst().name, QStringLiteral("newest"));
    QVERIFY(
        transport->requestForMethod(QStringLiteral("tools/list")).isEmpty());
}

void StdioMcpTransportTest::routesTypedNotificationsAndRichResults()
{
    using infrastructure::mcp::McpHostRuntime;

    QHash<QString, QSharedPointer<FakeMcpTransport>> transports;
    McpHostRuntime runtime(
        [&transports](const infrastructure::mcp::McpServerConfig& config)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            auto transport = QSharedPointer<FakeMcpTransport>::create();
            transports.insert(config.serverId, transport);
            return transport;
        });
    QString errorMessage;
    QVERIFY(runtime.addServer(runtimeTestConfig(QStringLiteral("alpha")),
                              errorMessage));
    runtime.startServer(QStringLiteral("alpha"));
    auto* transport = transports.value(QStringLiteral("alpha")).get();
    auto requestId = runtime.initialize(QStringLiteral("alpha"));
    transport->respond(requestId, initializeResult());
    requestId = runtime.listTools(QStringLiteral("alpha"));
    transport->respond(
        requestId,
        {{QStringLiteral("tools"),
          QJsonArray{QJsonObject{
              {QStringLiteral("name"), QStringLiteral("rich")},
              {QStringLiteral("inputSchema"),
               QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}},
              {QStringLiteral("outputSchema"),
               QJsonObject{
                   {QStringLiteral("type"), QStringLiteral("array")}}}}}}});

    QSignalSpy progressSpy(&runtime, &McpHostRuntime::progressReceived);
    QSignalSpy loggingSpy(&runtime, &McpHostRuntime::loggingMessageReceived);
    QSignalSpy resultSpy(&runtime, &McpHostRuntime::toolResultReady);
    QSignalSpy errorSpy(&runtime, &McpHostRuntime::serverError);
    transport->sendNotification(
        QStringLiteral("notifications/progress"),
        {{QStringLiteral("progressToken"), QStringLiteral("task-1")},
         {QStringLiteral("progress"), 1.0},
         {QStringLiteral("total"), 2.0},
         {QStringLiteral("message"), QStringLiteral("halfway")}});
    transport->sendNotification(
        QStringLiteral("notifications/message"),
        {{QStringLiteral("level"), QStringLiteral("info")},
         {QStringLiteral("logger"), QStringLiteral("fake")},
         {QStringLiteral("data"),
          QJsonObject{{QStringLiteral("message"), QStringLiteral("ready")}}}});
    QCOMPARE(progressSpy.count(), 1);
    QCOMPARE(loggingSpy.count(), 1);

    requestId = runtime.callTool(QStringLiteral("alpha.rich"), {});
    const QJsonArray structured{QJsonObject{{QStringLiteral("ok"), true}}};
    transport->respond(
        requestId,
        {{QStringLiteral("content"),
          QJsonArray{
              QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                          {QStringLiteral("text"), QStringLiteral("done")}},
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("custom-block")},
                  {QStringLiteral("value"), 42}}}},
         {QStringLiteral("structuredContent"), structured}});
    QCOMPARE(resultSpy.count(), 1);
    const auto result =
        qvariant_cast<agent::ToolResult>(resultSpy.constFirst().constFirst());
    QCOMPARE(result.structuredContent, QJsonValue(structured));
    QCOMPARE(result.contentBlocks.size(), 2);
    QCOMPARE(result.unknownContentBlockTypes,
             QStringList{QStringLiteral("custom-block")});
    QCOMPARE(errorSpy.count(), 0);

    requestId = runtime.callTool(QStringLiteral("alpha.rich"), {});
    transport->respond(requestId,
                       {{QStringLiteral("content"), QJsonArray{}},
                        {QStringLiteral("structuredContent"),
                         QJsonObject{{QStringLiteral("unexpected"), true}}}});
    QCOMPARE(resultSpy.count(), 2);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.constFirst().at(1).toString(),
             QStringLiteral("output_schema_mismatch"));
    const auto mismatchedResult =
        qvariant_cast<agent::ToolResult>(resultSpy.at(1).constFirst());
    QVERIFY(!mismatchedResult.isError);
    QVERIFY(mismatchedResult.structuredContent.isObject());
}

void StdioMcpTransportTest::limitsNotificationStorms()
{
    using infrastructure::mcp::McpHostRuntime;

    QHash<QString, QSharedPointer<FakeMcpTransport>> transports;
    McpHostRuntime runtime(
        [&transports](const infrastructure::mcp::McpServerConfig& config)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            auto transport = QSharedPointer<FakeMcpTransport>::create();
            transports.insert(config.serverId, transport);
            return transport;
        });
    QString errorMessage;
    QVERIFY(runtime.addServer(runtimeTestConfig(QStringLiteral("alpha")),
                              errorMessage));
    runtime.startServer(QStringLiteral("alpha"));
    auto* transport = transports.value(QStringLiteral("alpha")).get();
    QSignalSpy progressSpy(&runtime, &McpHostRuntime::progressReceived);
    QSignalSpy errorSpy(&runtime, &McpHostRuntime::serverError);

    for (auto index = 0; index < 150; ++index)
        transport->sendNotification(
            QStringLiteral("notifications/progress"),
            {{QStringLiteral("progressToken"), QStringLiteral("task")},
             {QStringLiteral("progress"), index}});

    QCOMPARE(progressSpy.count(), 100);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.constFirst().at(1).toString(),
             QStringLiteral("notification_rate_limited"));
}

void StdioMcpTransportTest::sendsProtocolCancellation()
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

    QSignalSpy notificationSpy(
        &manager, &infrastructure::mcp::McpClientManager::notificationReceived);
    const auto requestId =
        manager.callTool(QStringLiteral("fake.slow"), QJsonObject{});
    QVERIFY(!requestId.isEmpty());
    manager.cancel(requestId);
    QTRY_COMPARE_WITH_TIMEOUT(notificationSpy.count(), 1, 3'000);
    QCOMPARE(notificationSpy.constFirst().at(1).toString(),
             QStringLiteral("test/cancelled_seen"));
    QCOMPARE(notificationSpy.constFirst()
                 .at(2)
                 .toJsonObject()
                 .value(QStringLiteral("requestId"))
                 .toString(),
             requestId);
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
