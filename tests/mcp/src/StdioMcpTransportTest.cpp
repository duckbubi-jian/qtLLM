#include "BuiltInMcpServer.hpp"
#include "McpClientManager.hpp"
#include "McpHostRuntime.hpp"
#include "McpProtocol.hpp"
#include "McpServerRegistry.hpp"
#include "ToolPolicy.hpp"
#include "ToolRegistry.hpp"

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

    bool respond(const QJsonValue& requestId,
                 const QJsonObject& result) override
    {
        responses_.insert(requestId.toVariant().toString(), result);
        return running_;
    }

    bool respondError(const QJsonValue& requestId, int code,
                      const QString& message) override
    {
        errors_.insert(requestId.toVariant().toString(),
                       QJsonObject{{QStringLiteral("code"), code},
                                   {QStringLiteral("message"), message}});
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

    void sendRequest(const QJsonValue& requestId, const QString& method,
                     const QJsonObject& params = {})
    {
        emit requestReceived(requestId, method, params);
    }

    [[nodiscard]] QJsonObject response(const QString& requestId) const
    {
        return responses_.value(requestId);
    }

    [[nodiscard]] QJsonObject responseError(const QString& requestId) const
    {
        return errors_.value(requestId);
    }

    void crash()
    {
        running_ = false;
        emit transportError(QStringLiteral("crashed"),
                            QStringLiteral("Fake transport crashed."));
        emit stopped();
    }

    void failWhileRunning()
    {
        emit transportError(QStringLiteral("invalid_message"),
                            QStringLiteral("Fake protocol failure."));
    }

   private:
    bool running_ = false;
    int nextRequestId_ = 0;
    QHash<QString, QString> requests_;
    QHash<QString, QJsonObject> requestParams_;
    QHash<QString, QJsonObject> responses_;
    QHash<QString, QJsonObject> errors_;
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

QJsonObject catalogInitializeResult(bool listChanged = true)
{
    return {{QStringLiteral("protocolVersion"),
             infrastructure::mcp::latestSupportedProtocolVersion()},
            {QStringLiteral("capabilities"),
             QJsonObject{
                 {QStringLiteral("resources"),
                  QJsonObject{{QStringLiteral("subscribe"), true},
                              {QStringLiteral("listChanged"), listChanged}}},
                 {QStringLiteral("prompts"),
                  QJsonObject{{QStringLiteral("listChanged"), listChanged}}},
                 {QStringLiteral("logging"), QJsonObject{}},
                 {QStringLiteral("completions"), QJsonObject{}}}},
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
    void negotiatesSupportedProtocolVersions();
    void acceptsThirdPartyOutputStyles_data();
    void acceptsThirdPartyOutputStyles();
    void honorsProcessConfigurationAndWindowsWrapper();
    void rejectsInvalidThirdPartyOutput_data();
    void rejectsInvalidThirdPartyOutput();
    void keepsStderrDiagnosticsSeparateFromProtocol();
    void rejectsUnsupportedProtocolVersion();
    void rejectsUndeclaredToolsCapability();
    void propagatesRemoteErrors();
    void timesOutAndCanCancel();
    void validatesArgumentsBeforeCallingServer();
    void validatesDiscriminatedUnionArguments();
    void validatesDynamicPropertyNames();
    void appliesLocalToolPolicy();
    void roundTripsServerConfiguration();
    void rejectsInvalidServerIds();
    void builtInFilesystemWritesCppFile();
    void tracksServerLifecycleAndRevokesCapabilities();
    void restartsFailedRunningTransport();
    void isolatesMultipleServerFailures();
    void rejectsInvalidServerStateTransitions();
    void paginatesToolsAtomicallyAndPreservesMetadata();
    void refreshesToolsFromListChangedNotification();
    void routesTypedNotificationsAndRichResults();
    void limitsNotificationStorms();
    void sendsProtocolCancellation();
    void discoversReadsAndGetsMcpCatalogs();
    void completesCatalogArguments();
    void setsLoggingLevelsAndFiltersInstructions();
    void keepsIndependentCatalogFailuresDegraded();
    void pingsBothDirections();
    void servesOnlyAuthorizedRoots();
    void roundTripsRootsOverStdio();
    void roundTripsPingOverStdio();
    void roundTripsCompletionAndLoggingOverStdio();
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
             QStringLiteral("2025-06-18"));
    const QStringList supportedVersions{QStringLiteral("2025-06-18"),
                                        QStringLiteral("2025-03-26"),
                                        QStringLiteral("2024-11-05")};
    QCOMPARE(infrastructure::mcp::supportedProtocolVersions(),
             supportedVersions);
    for (const auto& version : supportedVersions)
        QVERIFY(infrastructure::mcp::isSupportedProtocolVersion(version));
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
        {QStringLiteral("protocolVersion"),
         infrastructure::mcp::latestSupportedProtocolVersion()},
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
    QCOMPARE(result.protocolVersion, QStringLiteral("2025-06-18"));
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

    for (const auto& version : supportedVersions)
    {
        auto versioned = initialize;
        versioned.insert(QStringLiteral("protocolVersion"), version);
        QVERIFY2(infrastructure::mcp::parseInitializeResult(versioned, result,
                                                            errorMessage),
                 qPrintable(errorMessage));
        QCOMPARE(result.protocolVersion, version);
    }

    auto invalid = initialize;
    invalid.insert(QStringLiteral("capabilities"),
                   QJsonObject{{QStringLiteral("tools"), true}});
    QVERIFY(!infrastructure::mcp::parseInitializeResult(invalid, result,
                                                        errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("tools")));
}

void StdioMcpTransportTest::negotiatesSupportedProtocolVersions()
{
    for (const auto& version : infrastructure::mcp::supportedProtocolVersions())
    {
        auto config = testConfig();
        config.serverId = QStringLiteral("fake-%1").arg(version);
        config.arguments.append(
            QStringLiteral("--protocol-version=%1").arg(version));
        infrastructure::mcp::McpClientManager manager;
        QString errorMessage;
        QVERIFY2(manager.addServer(config, errorMessage),
                 qPrintable(errorMessage));
        QSignalSpy initializedSpy(
            &manager,
            &infrastructure::mcp::McpClientManager::serverInitialized);

        manager.startServer(config.serverId);
        QVERIFY(!manager.initialize(config.serverId).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);
        const auto snapshot = manager.serverSnapshot(config.serverId);
        QVERIFY(snapshot.has_value());
        QCOMPARE(snapshot->protocolVersion, version);
        QCOMPARE(snapshot->state, infrastructure::mcp::McpServerState::Ready);
        manager.stopServer(config.serverId);
    }
}

void StdioMcpTransportTest::acceptsThirdPartyOutputStyles_data()
{
    QTest::addColumn<QStringList>("arguments");
    QTest::newRow("native-lf") << QStringList{};
    QTest::newRow("python-crlf") << QStringList{QStringLiteral("--crlf")};
    QTest::newRow("node-fragmented-crlf") << QStringList{
        QStringLiteral("--crlf"), QStringLiteral("--fragment-output")};
}

void StdioMcpTransportTest::acceptsThirdPartyOutputStyles()
{
    QFETCH(QStringList, arguments);
    auto config = testConfig();
    config.arguments = arguments;
    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY2(manager.addServer(config, errorMessage), qPrintable(errorMessage));
    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    QSignalSpy pingSpy(&manager,
                       &infrastructure::mcp::McpClientManager::pingCompleted);
    manager.startServer(config.serverId);
    QTRY_VERIFY_WITH_TIMEOUT(manager.transport(config.serverId)->isRunning(),
                             2'000);
    QVERIFY(!manager.initialize(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);
    QVERIFY(!manager.ping(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(pingSpy.count(), 1, 2'000);
}

void StdioMcpTransportTest::honorsProcessConfigurationAndWindowsWrapper()
{
    QTemporaryDir workingDirectory;
    QVERIFY(workingDirectory.isValid());
    auto config = testConfig();
    config.workingDirectory = workingDirectory.path();
    config.environment.insert(QStringLiteral("QTLLM_MCP_FIXTURE"),
                              QStringLiteral("configured"));
    config.arguments = {
        QStringLiteral("--require-cwd=%1").arg(workingDirectory.path()),
        QStringLiteral("--require-env=QTLLM_MCP_FIXTURE=configured")};

    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY2(manager.addServer(config, errorMessage), qPrintable(errorMessage));
    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    manager.startServer(config.serverId);
    QTRY_VERIFY_WITH_TIMEOUT(manager.transport(config.serverId)->isRunning(),
                             2'000);
    QVERIFY(!manager.initialize(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);

#ifdef Q_OS_WIN
    QTemporaryDir wrapperDirectory;
    QVERIFY(wrapperDirectory.isValid());
    const auto wrapperPath =
        QDir(wrapperDirectory.path()).filePath(QStringLiteral("fixture.cmd"));
    QFile wrapper(wrapperPath);
    QVERIFY(wrapper.open(QIODevice::WriteOnly | QIODevice::Text));
    const auto script = QByteArrayLiteral(
        "@echo off\r\n\"%QTLLM_TEST_MCP_SERVER%\" --crlf "
        "--fragment-output\r\n");
    QCOMPARE(wrapper.write(script), script.size());
    wrapper.close();

    auto wrapperConfig = testConfig();
    wrapperConfig.serverId = QStringLiteral("cmd-wrapper");
    wrapperConfig.program = qEnvironmentVariable("ComSpec");
    if (wrapperConfig.program.isEmpty())
        wrapperConfig.program = QDir::toNativeSeparators(
            QStringLiteral("C:/Windows/System32/cmd.exe"));
    wrapperConfig.arguments = {QStringLiteral("/d"), QStringLiteral("/s"),
                               QStringLiteral("/c"), QStringLiteral("call"),
                               QDir::toNativeSeparators(wrapperPath)};
    infrastructure::mcp::McpClientManager wrapperManager;
    QVERIFY2(wrapperManager.addServer(wrapperConfig, errorMessage),
             qPrintable(errorMessage));
    QSignalSpy wrapperInitialized(
        &wrapperManager,
        &infrastructure::mcp::McpClientManager::serverInitialized);
    wrapperManager.startServer(wrapperConfig.serverId);
    QTRY_VERIFY_WITH_TIMEOUT(
        wrapperManager.transport(wrapperConfig.serverId)->isRunning(), 2'000);
    QVERIFY(!wrapperManager.initialize(wrapperConfig.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(wrapperInitialized.count(), 1, 2'000);
    wrapperManager.stopServer(wrapperConfig.serverId);
    QTRY_VERIFY_WITH_TIMEOUT(
        !wrapperManager.transport(wrapperConfig.serverId)->isRunning(), 3'000);
#endif
}

void StdioMcpTransportTest::rejectsInvalidThirdPartyOutput_data()
{
    QTest::addColumn<QStringList>("arguments");
    QTest::addColumn<QString>("expectedCode");
    QTest::newRow("stdout-startup-noise")
        << QStringList{QStringLiteral("--startup-noise")}
        << QStringLiteral("invalid_message");
    QTest::newRow("oversized-stdout")
        << QStringList{QStringLiteral("--oversized-stdout")}
        << QStringLiteral("message_too_large");
    QTest::newRow("abnormal-exit")
        << QStringList{QStringLiteral("--exit-code=7")}
        << QStringLiteral("process_exited");
}

void StdioMcpTransportTest::rejectsInvalidThirdPartyOutput()
{
    QFETCH(QStringList, arguments);
    QFETCH(QString, expectedCode);
    auto config = testConfig();
    config.arguments = arguments;
    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY2(manager.addServer(config, errorMessage), qPrintable(errorMessage));
    QSignalSpy errorSpy(&manager,
                        &infrastructure::mcp::McpClientManager::serverError);
    manager.startServer(config.serverId);
    QTRY_VERIFY_WITH_TIMEOUT(errorSpy.count() >= 1, 3'000);
    auto found = false;
    for (const auto& values : errorSpy)
        if (values.at(1).toString() == expectedCode) found = true;
    QVERIFY2(found, qPrintable(expectedCode));
    const auto snapshot = manager.serverSnapshot(config.serverId);
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->state, infrastructure::mcp::McpServerState::Failed);

    auto healthy = testConfig();
    healthy.serverId = QStringLiteral("healthy");
    infrastructure::mcp::McpClientManager healthyManager;
    QVERIFY2(healthyManager.addServer(healthy, errorMessage),
             qPrintable(errorMessage));
    QSignalSpy initializedSpy(
        &healthyManager,
        &infrastructure::mcp::McpClientManager::serverInitialized);
    healthyManager.startServer(healthy.serverId);
    QTRY_VERIFY_WITH_TIMEOUT(
        healthyManager.transport(healthy.serverId)->isRunning(), 2'000);
    QVERIFY(!healthyManager.initialize(healthy.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);
}

void StdioMcpTransportTest::keepsStderrDiagnosticsSeparateFromProtocol()
{
    auto config = testConfig();
    config.arguments = {QStringLiteral("--stderr-lines=5000")};
    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY2(manager.addServer(config, errorMessage), qPrintable(errorMessage));
    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    QSignalSpy diagnosticSpy(
        &manager, &infrastructure::mcp::McpClientManager::diagnosticReceived);
    QSignalSpy errorSpy(&manager,
                        &infrastructure::mcp::McpClientManager::serverError);
    manager.startServer(config.serverId);
    QTRY_VERIFY_WITH_TIMEOUT(manager.transport(config.serverId)->isRunning(),
                             2'000);
    QVERIFY(!manager.initialize(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);
    QTRY_VERIFY_WITH_TIMEOUT(diagnosticSpy.count() >= 1, 2'000);
    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(diagnosticSpy.constFirst().at(1).toString().contains(
        QStringLiteral("fixture diagnostic")));
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
    QCOMPARE(result.outcome, agent::ToolOutcome::Succeeded);
    QCOMPARE(result.sideEffectState, agent::ToolSideEffectState::Succeeded);

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
    QCOMPARE(remoteError.outcome, agent::ToolOutcome::ServerFailed);
    QCOMPARE(remoteError.sideEffectState,
             agent::ToolSideEffectState::Uncertain);

    QVERIFY(
        !manager.callTool(QStringLiteral("fake.business_error"), {}).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 2, 2'000);
    const auto businessError =
        qvariant_cast<agent::ToolResult>(resultSpy.at(1).at(0));
    QVERIFY(businessError.isError);
    QCOMPARE(businessError.failureKind, agent::ToolFailureKind::Tool);
    QCOMPARE(businessError.outcome, agent::ToolOutcome::ToolFailed);
    QCOMPARE(businessError.sideEffectState,
             agent::ToolSideEffectState::KnownFailed);
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
    QSignalSpy resultSpy(
        &manager, &infrastructure::mcp::McpClientManager::toolResultReady);
    const auto requestId = manager.callTool(QStringLiteral("fake.slow"), {});
    QVERIFY(!requestId.isEmpty());
    manager.cancel(requestId);
    QTRY_COMPARE_WITH_TIMEOUT(failureSpy.count(), 1, 1'000);
    QCOMPARE(failureSpy.at(0).at(3).toString(), QStringLiteral("cancelled"));
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 1, 1'000);
    const auto cancelled =
        qvariant_cast<agent::ToolResult>(resultSpy.at(0).at(0));
    QCOMPARE(cancelled.outcome, agent::ToolOutcome::Cancelled);
    QCOMPARE(cancelled.sideEffectState, agent::ToolSideEffectState::Uncertain);

    const auto timeoutRequestId =
        manager.callTool(QStringLiteral("fake.slow"), {});
    QVERIFY(!timeoutRequestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(failureSpy.count(), 2, 3'000);
    QCOMPARE(failureSpy.at(1).at(3).toString(), QStringLiteral("timeout"));
    QTRY_COMPARE_WITH_TIMEOUT(resultSpy.count(), 2, 3'000);
    const auto timedOut =
        qvariant_cast<agent::ToolResult>(resultSpy.at(1).at(0));
    QCOMPARE(timedOut.outcome, agent::ToolOutcome::TransportFailed);
    QCOMPARE(timedOut.sideEffectState, agent::ToolSideEffectState::Uncertain);
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

void StdioMcpTransportTest::validatesDiscriminatedUnionArguments()
{
    const QJsonObject geometrySource{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("type"),
              QJsonObject{
                  {QStringLiteral("const"), QStringLiteral("geometry_file")}}},
             {QStringLiteral("file_path"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("scale_ratio"),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("number")}}}}},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("type"), QStringLiteral("file_path")}}};
    const QJsonObject circleSource{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("type"),
              QJsonObject{{QStringLiteral("const"), QStringLiteral("circle")}}},
             {QStringLiteral("radius"),
              QJsonObject{{QStringLiteral("type"),
                           QJsonArray{QStringLiteral("number"),
                                      QStringLiteral("null")}}}}}},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("type"), QStringLiteral("radius")}}};
    const QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("source"),
              QJsonObject{
                  {QStringLiteral("discriminator"),
                   QJsonObject{{QStringLiteral("propertyName"),
                                QStringLiteral("type")}}},
                  {QStringLiteral("oneOf"),
                   QJsonArray{QJsonObject{{QStringLiteral("$ref"),
                                           QStringLiteral(
                                               "#/$defs/GeometryFileSource")}},
                              QJsonObject{{QStringLiteral("$ref"),
                                           QStringLiteral(
                                               "#/$defs/CircleSource")}}}}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("source")}},
        {QStringLiteral("$defs"),
         QJsonObject{{QStringLiteral("GeometryFileSource"), geometrySource},
                     {QStringLiteral("CircleSource"), circleSource}}}};
    const agent::ToolDefinition tool{
        QStringLiteral("fake.create_inlet"), QStringLiteral("fake"),
        QStringLiteral("create_inlet"), QStringLiteral("Create inlet"), schema};
    infrastructure::mcp::ToolRegistry registry;
    QString errorMessage;
    QVERIFY(registry.replaceServerTools(QStringLiteral("fake"), {tool},
                                        errorMessage));

    const auto wrongDiscriminator = registry.validateArgumentsDetailed(
        QStringLiteral("fake.create_inlet"),
        {{QStringLiteral("source"),
          QJsonObject{
              {QStringLiteral("type"), QStringLiteral("geometryFile")},
              {QStringLiteral("file_path"), QStringLiteral("E:/inlet.stl")}}}});
    QVERIFY(!wrongDiscriminator.valid);
    QCOMPARE(wrongDiscriminator.issue.toolName,
             QStringLiteral("fake.create_inlet"));
    QCOMPARE(wrongDiscriminator.issue.instancePath,
             QStringLiteral("arguments.source.type"));
    QCOMPARE(wrongDiscriminator.issue.schemaPath,
             QStringLiteral("#/properties/source/oneOf"));
    QCOMPARE(wrongDiscriminator.issue.keyword, QStringLiteral("discriminator"));
    QVERIFY(wrongDiscriminator.issue.message.contains(
        QStringLiteral("geometry_file")));
    QVERIFY(
        wrongDiscriminator.issue.message.contains(QStringLiteral("circle")));

    const auto invalidScaleRatio = registry.validateArgumentsDetailed(
        QStringLiteral("fake.create_inlet"),
        {{QStringLiteral("source"),
          QJsonObject{
              {QStringLiteral("type"), QStringLiteral("geometry_file")},
              {QStringLiteral("file_path"), QStringLiteral("E:/inlet.stl")},
              {QStringLiteral("scale_ratio"), QJsonValue(QJsonValue::Null)}}}});
    QVERIFY(!invalidScaleRatio.valid);
    QCOMPARE(invalidScaleRatio.issue.instancePath,
             QStringLiteral("arguments.source.scale_ratio"));
    QCOMPARE(invalidScaleRatio.issue.schemaPath,
             QStringLiteral(
                 "#/$defs/GeometryFileSource/properties/scale_ratio/type"));
    QCOMPARE(invalidScaleRatio.issue.keyword, QStringLiteral("type"));
    QVERIFY(invalidScaleRatio.issue.message.contains(
        QStringLiteral("arguments.source.scale_ratio must be number")));

    const auto missingCircleRadius = registry.validateArgumentsDetailed(
        QStringLiteral("fake.create_inlet"),
        {{QStringLiteral("source"),
          QJsonObject{{QStringLiteral("type"), QStringLiteral("circle")}}}});
    QVERIFY(!missingCircleRadius.valid);
    QCOMPARE(missingCircleRadius.issue.instancePath,
             QStringLiteral("arguments.source.radius"));
    QCOMPARE(missingCircleRadius.issue.schemaPath,
             QStringLiteral("#/$defs/CircleSource/required"));
    QCOMPARE(missingCircleRadius.issue.keyword, QStringLiteral("required"));

    const auto unknownTool =
        registry.validateArgumentsDetailed(QStringLiteral("fake.missing"), {});
    QVERIFY(!unknownTool.valid);
    QCOMPARE(unknownTool.issue.toolName, QStringLiteral("fake.missing"));
    QCOMPARE(unknownTool.issue.instancePath, QStringLiteral("arguments"));
    QCOMPARE(unknownTool.issue.keyword, QStringLiteral("tool"));

    errorMessage.clear();
    QVERIFY(registry.validateArguments(
        QStringLiteral("fake.create_inlet"),
        {{QStringLiteral("source"),
          QJsonObject{
              {QStringLiteral("type"), QStringLiteral("geometry_file")},
              {QStringLiteral("file_path"), QStringLiteral("E:/inlet.stl")}}}},
        errorMessage));
    QVERIFY(errorMessage.isEmpty());

    QVERIFY(registry.validateArguments(
        QStringLiteral("fake.create_inlet"),
        {{QStringLiteral("source"),
          QJsonObject{
              {QStringLiteral("type"), QStringLiteral("circle")},
              {QStringLiteral("radius"), QJsonValue(QJsonValue::Null)}}}},
        errorMessage));
}

void StdioMcpTransportTest::validatesDynamicPropertyNames()
{
    const QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("updates"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                          {QStringLiteral("minProperties"), 1},
                          {QStringLiteral("propertyNames"),
                           QJsonObject{{QStringLiteral("pattern"),
                                        QStringLiteral("^/")}}},
                          {QStringLiteral("additionalProperties"),
                           QJsonObject{{QStringLiteral("type"),
                                        QStringLiteral("number")}}}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("updates")}}};
    const agent::ToolDefinition tool{QStringLiteral("fake.update_fields"),
                                     QStringLiteral("fake"),
                                     QStringLiteral("update_fields"),
                                     QStringLiteral("Update fields"), schema};
    infrastructure::mcp::ToolRegistry registry;
    QString errorMessage;
    QVERIFY(registry.replaceServerTools(QStringLiteral("fake"), {tool},
                                        errorMessage));

    const auto empty = registry.validateArgumentsDetailed(
        QStringLiteral("fake.update_fields"),
        {{QStringLiteral("updates"), QJsonObject{}}});
    QVERIFY(!empty.valid);
    QCOMPARE(empty.issue.keyword, QStringLiteral("minProperties"));

    const auto invalid = registry.validateArgumentsDetailed(
        QStringLiteral("fake.update_fields"),
        {{QStringLiteral("updates"),
          QJsonObject{{QStringLiteral("~1density~1fixedValue"), 900.0}}}});
    QVERIFY(!invalid.valid);
    QCOMPARE(invalid.issue.instancePath, QStringLiteral("arguments.updates"));
    QCOMPARE(invalid.issue.schemaPath,
             QStringLiteral("#/properties/updates/propertyNames/pattern"));
    QCOMPARE(invalid.issue.keyword, QStringLiteral("propertyNames"));
    QVERIFY(invalid.issue.message.contains(QStringLiteral("must match")));

    const auto valid = registry.validateArgumentsDetailed(
        QStringLiteral("fake.update_fields"),
        {{QStringLiteral("updates"),
          QJsonObject{{QStringLiteral("/density/fixedValue"), 900.0}}}});
    QVERIFY(valid.valid);

    const auto invalidValue = registry.validateArgumentsDetailed(
        QStringLiteral("fake.update_fields"),
        {{QStringLiteral("updates"),
          QJsonObject{{QStringLiteral("/density/fixedValue"),
                       QStringLiteral("900")}}}});
    QVERIFY(!invalidValue.valid);
    QCOMPARE(invalidValue.issue.instancePath,
             QStringLiteral("arguments.updates./density/fixedValue"));
    QCOMPARE(invalidValue.issue.schemaPath,
             QStringLiteral("#/properties/updates/additionalProperties/type"));
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
    const auto unknownDomainRule = infrastructure::mcp::defaultToolPolicyRule(
        QStringLiteral("domain-server.change_context"));
    QCOMPARE(unknownDomainRule.risk,
             infrastructure::mcp::ToolRisk::ModifiesData);
    QVERIFY(!unknownDomainRule.alwaysAllow);
    const auto annotatedReadRule = infrastructure::mcp::defaultToolPolicyRule(
        QStringLiteral("domain-server.inspect_state"),
        {{QStringLiteral("readOnlyHint"), true}});
    QCOMPARE(annotatedReadRule.risk, infrastructure::mcp::ToolRisk::ReadOnly);
    QVERIFY(!annotatedReadRule.alwaysAllow);
    const auto annotatedDestructiveRule =
        infrastructure::mcp::defaultToolPolicyRule(
            QStringLiteral("domain-server.change_state"),
            {{QStringLiteral("destructiveHint"), true}});
    QCOMPARE(annotatedDestructiveRule.risk,
             infrastructure::mcp::ToolRisk::Destructive);
}

void StdioMcpTransportTest::roundTripsServerConfiguration()
{
    auto config = testConfig();
    config.arguments = {QStringLiteral("--flag"), QStringLiteral("value")};
    config.environment.insert(QStringLiteral("TOKEN"),
                              QStringLiteral("not-a-real-secret"));
    config.workingDirectory = QStringLiteral("D:/workspace");
    config.toolAllowlist = {QStringLiteral("echo")};
    config.authorizedRoots = {QDir::rootPath()};
    config.maxResultBytes = 8'192;
    config.loggingLevel = QStringLiteral("warning");
    config.useInstructions = false;

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
    QCOMPARE(parsed.authorizedRoots, config.authorizedRoots);
    QCOMPARE(parsed.maxResultBytes, config.maxResultBytes);
    QCOMPARE(parsed.loggingLevel, config.loggingLevel);
    QCOMPARE(parsed.useInstructions, config.useInstructions);

    auto legacy = infrastructure::mcp::serializeServerConfig(config);
    legacy.remove(QStringLiteral("loggingLevel"));
    legacy.remove(QStringLiteral("useInstructions"));
    QVERIFY2(
        infrastructure::mcp::parseServerConfig(legacy, parsed, errorMessage),
        qPrintable(errorMessage));
    QVERIFY(parsed.loggingLevel.isEmpty());
    QVERIFY(parsed.useInstructions);
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

void StdioMcpTransportTest::restartsFailedRunningTransport()
{
    using infrastructure::mcp::McpHostRuntime;
    using infrastructure::mcp::McpServerState;

    QSharedPointer<FakeMcpTransport> transport;
    McpHostRuntime runtime(
        [&transport](const infrastructure::mcp::McpServerConfig&)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            transport = QSharedPointer<FakeMcpTransport>::create();
            return transport;
        });
    QString errorMessage;
    QVERIFY2(runtime.addServer(runtimeTestConfig(QStringLiteral("alpha")),
                               errorMessage),
             qPrintable(errorMessage));
    runtime.startServer(QStringLiteral("alpha"));
    QVERIFY(transport->isRunning());

    transport->failWhileRunning();
    QCOMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->state,
             McpServerState::Failed);
    QVERIFY(transport->isRunning());

    QSignalSpy stoppedSpy(&runtime, &McpHostRuntime::serverStopped);
    QSignalSpy startedSpy(&runtime, &McpHostRuntime::serverStarted);
    runtime.startServer(QStringLiteral("alpha"));
    QCOMPARE(stoppedSpy.count(), 1);
    QTRY_COMPARE(runtime.serverSnapshot(QStringLiteral("alpha"))->state,
                 McpServerState::Starting);
    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(transport->isRunning());
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

void StdioMcpTransportTest::discoversReadsAndGetsMcpCatalogs()
{
    using infrastructure::mcp::McpHostRuntime;

    QSharedPointer<FakeMcpTransport> transport;
    McpHostRuntime runtime(
        [&transport](const infrastructure::mcp::McpServerConfig&)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            transport = QSharedPointer<FakeMcpTransport>::create();
            return transport;
        });
    QString errorMessage;
    QVERIFY2(runtime.addServer(runtimeTestConfig(QStringLiteral("alpha")),
                               errorMessage),
             qPrintable(errorMessage));
    runtime.startServer(QStringLiteral("alpha"));
    const auto initializeRequest = runtime.initialize(QStringLiteral("alpha"));
    transport->respond(initializeRequest, catalogInitializeResult());

    QSignalSpy resourcesSpy(&runtime, &McpHostRuntime::resourcesChanged);
    auto requestId = runtime.listResources(QStringLiteral("alpha"));
    transport->respond(
        requestId,
        {{QStringLiteral("resources"),
          QJsonArray{QJsonObject{
              {QStringLiteral("uri"), QStringLiteral("test://resource/one")},
              {QStringLiteral("name"), QStringLiteral("One")},
              {QStringLiteral("mimeType"), QStringLiteral("text/plain")}}}},
         {QStringLiteral("nextCursor"), QStringLiteral("page-2")}});
    QVERIFY(runtime.resources().isEmpty());
    const auto secondResourcePage =
        transport->requestForMethod(QStringLiteral("resources/list"));
    QVERIFY(!secondResourcePage.isEmpty());
    QCOMPARE(transport->requestParams(secondResourcePage)
                 .value(QStringLiteral("cursor"))
                 .toString(),
             QStringLiteral("page-2"));
    transport->respond(
        secondResourcePage,
        {{QStringLiteral("resources"),
          QJsonArray{QJsonObject{
              {QStringLiteral("uri"), QStringLiteral("test://resource/two")},
              {QStringLiteral("name"), QStringLiteral("Two")}}}}});
    QCOMPARE(resourcesSpy.count(), 1);
    QCOMPARE(runtime.resources(QStringLiteral("alpha")).size(), 2);

    QSignalSpy templatesSpy(&runtime,
                            &McpHostRuntime::resourceTemplatesChanged);
    requestId = runtime.listResourceTemplates(QStringLiteral("alpha"));
    transport->respond(
        requestId, {{QStringLiteral("resourceTemplates"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("uriTemplate"),
                          QStringLiteral("test://resource/{name}")},
                         {QStringLiteral("name"), QStringLiteral("Named")}}}}});
    QCOMPARE(templatesSpy.count(), 1);
    QCOMPARE(runtime.resourceTemplates(QStringLiteral("alpha")).size(), 1);

    QSignalSpy promptsSpy(&runtime, &McpHostRuntime::promptsChanged);
    requestId = runtime.listPrompts(QStringLiteral("alpha"));
    transport->respond(
        requestId,
        {{QStringLiteral("prompts"),
          QJsonArray{QJsonObject{
              {QStringLiteral("name"), QStringLiteral("summarize")},
              {QStringLiteral("description"), QStringLiteral("Summarize")},
              {QStringLiteral("arguments"),
               QJsonArray{QJsonObject{
                   {QStringLiteral("name"), QStringLiteral("topic")},
                   {QStringLiteral("required"), true}}}}}}}});
    QCOMPARE(promptsSpy.count(), 1);
    QCOMPARE(runtime.prompts(QStringLiteral("alpha")).size(), 1);

    const auto resourceCatalog = QJsonObject{
        {QStringLiteral("resources"),
         QJsonArray{
             QJsonObject{
                 {QStringLiteral("uri"), QStringLiteral("test://resource/one")},
                 {QStringLiteral("name"), QStringLiteral("One")}},
             QJsonObject{
                 {QStringLiteral("uri"), QStringLiteral("test://resource/two")},
                 {QStringLiteral("name"), QStringLiteral("Two")}}}}};
    const auto templateCatalog =
        QJsonObject{{QStringLiteral("resourceTemplates"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("uriTemplate"),
                          QStringLiteral("test://resource/{name}")},
                         {QStringLiteral("name"), QStringLiteral("Named")}}}}};
    transport->sendNotification(
        QStringLiteral("notifications/resources/list_changed"), {});
    const auto firstResourceRefresh =
        transport->requestForMethod(QStringLiteral("resources/list"));
    const auto firstTemplateRefresh =
        transport->requestForMethod(QStringLiteral("resources/templates/list"));
    QVERIFY(!firstResourceRefresh.isEmpty());
    QVERIFY(!firstTemplateRefresh.isEmpty());
    transport->sendNotification(
        QStringLiteral("notifications/resources/list_changed"), {});
    transport->respond(firstResourceRefresh, resourceCatalog);
    transport->respond(firstTemplateRefresh, templateCatalog);
    const auto secondResourceRefresh =
        transport->requestForMethod(QStringLiteral("resources/list"));
    const auto secondTemplateRefresh =
        transport->requestForMethod(QStringLiteral("resources/templates/list"));
    QVERIFY(!secondResourceRefresh.isEmpty());
    QVERIFY(!secondTemplateRefresh.isEmpty());
    transport->respond(secondResourceRefresh, resourceCatalog);
    transport->respond(secondTemplateRefresh, templateCatalog);
    QCOMPARE(resourcesSpy.count(), 3);
    QCOMPARE(templatesSpy.count(), 3);

    const auto promptCatalog =
        QJsonObject{{QStringLiteral("prompts"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("name"), QStringLiteral("summarize")},
                         {QStringLiteral("arguments"),
                          QJsonArray{QJsonObject{
                              {QStringLiteral("name"), QStringLiteral("topic")},
                              {QStringLiteral("required"), true}}}}}}}};
    transport->sendNotification(
        QStringLiteral("notifications/prompts/list_changed"), {});
    const auto firstPromptRefresh =
        transport->requestForMethod(QStringLiteral("prompts/list"));
    QVERIFY(!firstPromptRefresh.isEmpty());
    transport->sendNotification(
        QStringLiteral("notifications/prompts/list_changed"), {});
    transport->respond(firstPromptRefresh, promptCatalog);
    const auto secondPromptRefresh =
        transport->requestForMethod(QStringLiteral("prompts/list"));
    QVERIFY(!secondPromptRefresh.isEmpty());
    transport->respond(secondPromptRefresh, promptCatalog);
    QCOMPARE(promptsSpy.count(), 3);

    QSignalSpy failureSpy(&runtime, &McpHostRuntime::requestFailed);
    QVERIFY(
        runtime
            .getPrompt(QStringLiteral("alpha"), QStringLiteral("summarize"), {})
            .isEmpty());
    QCOMPARE(failureSpy.count(), 1);
    QVERIFY(failureSpy.constFirst().at(4).toString().contains(
        QStringLiteral("required")));

    QSignalSpy resourceResultSpy(&runtime, &McpHostRuntime::resourceReadReady);
    requestId = runtime.readResource(QStringLiteral("alpha"),
                                     QStringLiteral("test://resource/one"));
    transport->respond(
        requestId,
        {{QStringLiteral("contents"),
          QJsonArray{QJsonObject{
              {QStringLiteral("uri"), QStringLiteral("test://resource/one")},
              {QStringLiteral("mimeType"), QStringLiteral("text/plain")},
              {QStringLiteral("text"), QStringLiteral("resource text")}}}}});
    QCOMPARE(resourceResultSpy.count(), 1);
    const auto resourceResult =
        qvariant_cast<infrastructure::mcp::McpResourceReadResult>(
            resourceResultSpy.constFirst().constFirst());
    QCOMPARE(resourceResult.contents.size(), 1);
    QCOMPARE(resourceResult.contents.constFirst().text,
             QStringLiteral("resource text"));

    QSignalSpy promptResultSpy(&runtime, &McpHostRuntime::promptReady);
    requestId =
        runtime.getPrompt(QStringLiteral("alpha"), QStringLiteral("summarize"),
                          {{QStringLiteral("topic"), QStringLiteral("MCP")}});
    transport->respond(
        requestId,
        {{QStringLiteral("description"), QStringLiteral("Generated")},
         {QStringLiteral("messages"),
          QJsonArray{QJsonObject{
              {QStringLiteral("role"), QStringLiteral("user")},
              {QStringLiteral("content"),
               QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                           {QStringLiteral("text"),
                            QStringLiteral("Summarize MCP")}}}}}}});
    QCOMPARE(promptResultSpy.count(), 1);
    const auto promptResult =
        qvariant_cast<infrastructure::mcp::McpPromptResult>(
            promptResultSpy.constFirst().constFirst());
    QCOMPARE(promptResult.messages.size(), 1);
    QCOMPARE(promptResult.messages.constFirst().role, QStringLiteral("user"));

    QSignalSpy subscriptionSpy(&runtime,
                               &McpHostRuntime::resourceSubscriptionChanged);
    requestId = runtime.subscribeResource(
        QStringLiteral("alpha"), QStringLiteral("test://resource/one"));
    transport->respond(requestId, {});
    QCOMPARE(subscriptionSpy.count(), 1);
    QSignalSpy updatedSpy(&runtime, &McpHostRuntime::resourceUpdated);
    transport->sendNotification(
        QStringLiteral("notifications/resources/updated"),
        {{QStringLiteral("uri"), QStringLiteral("test://resource/one")}});
    QCOMPARE(updatedSpy.count(), 1);

    const auto snapshot = runtime.serverSnapshot(QStringLiteral("alpha"));
    QCOMPARE(snapshot->resourceCount, 2);
    QCOMPARE(snapshot->resourceTemplateCount, 1);
    QCOMPARE(snapshot->promptCount, 1);

    runtime.stopServer(QStringLiteral("alpha"));
    QVERIFY(runtime.resources().isEmpty());
    QVERIFY(runtime.resourceTemplates().isEmpty());
    QVERIFY(runtime.prompts().isEmpty());
}

void StdioMcpTransportTest::completesCatalogArguments()
{
    using infrastructure::mcp::McpCompletionResult;
    using infrastructure::mcp::McpHostRuntime;

    QSharedPointer<FakeMcpTransport> transport;
    McpHostRuntime runtime(
        [&transport](const infrastructure::mcp::McpServerConfig&)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            transport = QSharedPointer<FakeMcpTransport>::create();
            return transport;
        });
    QString errorMessage;
    QVERIFY2(runtime.addServer(runtimeTestConfig(QStringLiteral("alpha")),
                               errorMessage),
             qPrintable(errorMessage));
    runtime.startServer(QStringLiteral("alpha"));
    const auto initializeRequest = runtime.initialize(QStringLiteral("alpha"));
    transport->respond(initializeRequest, catalogInitializeResult());

    auto requestId = runtime.listPrompts(QStringLiteral("alpha"));
    transport->respond(
        requestId,
        {{QStringLiteral("prompts"),
          QJsonArray{QJsonObject{
              {QStringLiteral("name"), QStringLiteral("summarize")},
              {QStringLiteral("arguments"),
               QJsonArray{QJsonObject{
                   {QStringLiteral("name"), QStringLiteral("topic")}}}}}}}});
    requestId = runtime.listResourceTemplates(QStringLiteral("alpha"));
    transport->respond(
        requestId, {{QStringLiteral("resourceTemplates"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("uriTemplate"),
                          QStringLiteral("test://resource/{name}")},
                         {QStringLiteral("name"), QStringLiteral("Named")}}}}});

    QSignalSpy completionSpy(&runtime, &McpHostRuntime::completionReady);
    requestId = runtime.completePrompt(
        QStringLiteral("alpha"), QStringLiteral("summarize"),
        QStringLiteral("topic"), QStringLiteral("mc"),
        {{QStringLiteral("language"), QStringLiteral("en")}});
    QVERIFY(!requestId.isEmpty());
    const auto promptParams = transport->requestParams(requestId);
    QCOMPARE(promptParams.value(QStringLiteral("ref"))
                 .toObject()
                 .value(QStringLiteral("type"))
                 .toString(),
             QStringLiteral("ref/prompt"));
    QCOMPARE(promptParams.value(QStringLiteral("ref"))
                 .toObject()
                 .value(QStringLiteral("name"))
                 .toString(),
             QStringLiteral("summarize"));
    QCOMPARE(promptParams.value(QStringLiteral("context"))
                 .toObject()
                 .value(QStringLiteral("arguments"))
                 .toObject()
                 .value(QStringLiteral("language"))
                 .toString(),
             QStringLiteral("en"));
    transport->respond(
        requestId,
        {{QStringLiteral("completion"),
          QJsonObject{
              {QStringLiteral("values"),
               QJsonArray{QStringLiteral("mcp"), QStringLiteral("mcp host")}},
              {QStringLiteral("total"), 3},
              {QStringLiteral("hasMore"), true},
              {QStringLiteral("vendorField"), QStringLiteral("preserved")}}}});
    QCOMPARE(completionSpy.count(), 1);
    const auto promptResult = qvariant_cast<McpCompletionResult>(
        completionSpy.constFirst().constFirst());
    QCOMPARE(promptResult.values,
             QStringList({QStringLiteral("mcp"), QStringLiteral("mcp host")}));
    QCOMPARE(promptResult.total, 3);
    QVERIFY(promptResult.totalProvided);
    QVERIFY(promptResult.hasMoreProvided);
    QVERIFY(promptResult.hasMore);
    QCOMPARE(promptResult.rawCompletion.value(QStringLiteral("vendorField"))
                 .toString(),
             QStringLiteral("preserved"));

    requestId = runtime.completeResourceTemplate(
        QStringLiteral("alpha"), QStringLiteral("test://resource/{name}"),
        QStringLiteral("name"), QStringLiteral("read"));
    QVERIFY(!requestId.isEmpty());
    QCOMPARE(transport->requestParams(requestId)
                 .value(QStringLiteral("ref"))
                 .toObject()
                 .value(QStringLiteral("uri"))
                 .toString(),
             QStringLiteral("test://resource/{name}"));
    transport->respond(requestId,
                       {{QStringLiteral("completion"),
                         QJsonObject{{QStringLiteral("values"),
                                      QJsonArray{QStringLiteral("readme")}}}}});
    QCOMPARE(completionSpy.count(), 2);

    QSignalSpy failureSpy(&runtime, &McpHostRuntime::requestFailed);
    requestId = runtime.completePrompt(
        QStringLiteral("alpha"), QStringLiteral("summarize"),
        QStringLiteral("topic"), QStringLiteral("cancel"));
    QVERIFY(!requestId.isEmpty());
    runtime.cancel(requestId);
    QCOMPARE(failureSpy.count(), 1);
    QCOMPARE(failureSpy.constFirst().at(3).toString(),
             QStringLiteral("cancelled"));
    transport->respond(
        requestId, {{QStringLiteral("completion"),
                     QJsonObject{{QStringLiteral("values"), QJsonArray{}}}}});
    QCOMPARE(completionSpy.count(), 2);

    requestId = runtime.completePrompt(
        QStringLiteral("alpha"), QStringLiteral("summarize"),
        QStringLiteral("topic"), QStringLiteral("large"));
    QVERIFY(!requestId.isEmpty());
    transport->respond(
        requestId,
        {{QStringLiteral("completion"),
          QJsonObject{{QStringLiteral("values"),
                       QJsonArray{QString(70'000, QLatin1Char('x'))}}}}});
    QCOMPARE(failureSpy.count(), 2);
    QCOMPARE(failureSpy.constLast().at(3).toString(),
             QStringLiteral("invalid_response"));
    QCOMPARE(completionSpy.count(), 2);

    QVERIFY(runtime
                .completePrompt(QStringLiteral("alpha"),
                                QStringLiteral("summarize"),
                                QStringLiteral("unknown"), QString{})
                .isEmpty());
    QCOMPARE(failureSpy.count(), 3);
}

void StdioMcpTransportTest::setsLoggingLevelsAndFiltersInstructions()
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
    auto alpha = runtimeTestConfig(QStringLiteral("alpha"));
    alpha.loggingLevel = QStringLiteral("notice");
    auto beta = runtimeTestConfig(QStringLiteral("beta"));
    beta.useInstructions = false;
    QString errorMessage;
    QVERIFY2(runtime.addServer(alpha, errorMessage), qPrintable(errorMessage));
    QVERIFY2(runtime.addServer(beta, errorMessage), qPrintable(errorMessage));

    QSignalSpy levelSpy(&runtime, &McpHostRuntime::loggingLevelChanged);
    for (const auto& serverId :
         {QStringLiteral("alpha"), QStringLiteral("beta")})
    {
        runtime.startServer(serverId);
        const auto requestId = runtime.initialize(serverId);
        auto initialized = catalogInitializeResult();
        initialized.insert(
            QStringLiteral("instructions"),
            QStringLiteral("Instructions from %1").arg(serverId));
        transports.value(serverId)->respond(requestId, initialized);
    }
    const auto replayRequest =
        transports.value(QStringLiteral("alpha"))
            ->requestForMethod(QStringLiteral("logging/setLevel"));
    QVERIFY(!replayRequest.isEmpty());
    QCOMPARE(transports.value(QStringLiteral("alpha"))
                 ->requestParams(replayRequest)
                 .value(QStringLiteral("level"))
                 .toString(),
             QStringLiteral("notice"));
    transports.value(QStringLiteral("alpha"))->respond(replayRequest, {});
    QCOMPARE(levelSpy.count(), 1);

    QVERIFY(runtime.agentInstructions().contains(QStringLiteral("alpha:")));
    QVERIFY(!runtime.agentInstructions().contains(QStringLiteral("beta:")));
    QVERIFY(
        runtime.setUseInstructions(QStringLiteral("beta"), true, errorMessage));
    QVERIFY(runtime.agentInstructions().contains(QStringLiteral("beta:")));
    QVERIFY(runtime.setUseInstructions(QStringLiteral("alpha"), false,
                                       errorMessage));
    QVERIFY(!runtime.agentInstructions().contains(QStringLiteral("alpha:")));

    auto requestId = runtime.setLoggingLevel(QStringLiteral("beta"),
                                             QStringLiteral("warning"));
    QVERIFY(!requestId.isEmpty());
    QCOMPARE(transports.value(QStringLiteral("beta"))
                 ->requestParams(requestId)
                 .value(QStringLiteral("level"))
                 .toString(),
             QStringLiteral("warning"));
    transports.value(QStringLiteral("beta"))->respond(requestId, {});
    QCOMPARE(levelSpy.count(), 2);
    QCOMPARE(levelSpy.constLast().at(0).toString(), QStringLiteral("beta"));
    QCOMPARE(levelSpy.constLast().at(1).toString(), QStringLiteral("warning"));

    QSignalSpy failureSpy(&runtime, &McpHostRuntime::requestFailed);
    QVERIFY(
        runtime
            .setLoggingLevel(QStringLiteral("beta"), QStringLiteral("verbose"))
            .isEmpty());
    QCOMPARE(failureSpy.count(), 1);

    auto gamma = runtimeTestConfig(QStringLiteral("gamma"));
    QVERIFY2(runtime.addServer(gamma, errorMessage), qPrintable(errorMessage));
    runtime.startServer(QStringLiteral("gamma"));
    requestId = runtime.initialize(QStringLiteral("gamma"));
    transports.value(QStringLiteral("gamma"))
        ->respond(requestId, initializeResult());
    QVERIFY(
        runtime.setLoggingLevel(QStringLiteral("gamma"), QStringLiteral("info"))
            .isEmpty());
    QCOMPARE(failureSpy.count(), 2);
    QVERIFY(transports.value(QStringLiteral("gamma"))
                ->requestForMethod(QStringLiteral("logging/setLevel"))
                .isEmpty());
}

void StdioMcpTransportTest::keepsIndependentCatalogFailuresDegraded()
{
    using infrastructure::mcp::McpHostRuntime;
    using infrastructure::mcp::McpServerState;

    QSharedPointer<FakeMcpTransport> transport;
    McpHostRuntime runtime(
        [&transport](const infrastructure::mcp::McpServerConfig&)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            transport = QSharedPointer<FakeMcpTransport>::create();
            return transport;
        });
    QString errorMessage;
    QVERIFY2(runtime.addServer(runtimeTestConfig(QStringLiteral("alpha")),
                               errorMessage),
             qPrintable(errorMessage));
    runtime.startServer(QStringLiteral("alpha"));
    const auto initializeRequest = runtime.initialize(QStringLiteral("alpha"));
    transport->respond(initializeRequest, catalogInitializeResult());

    const auto resourceRequest = runtime.listResources(QStringLiteral("alpha"));
    const auto promptRequest = runtime.listPrompts(QStringLiteral("alpha"));
    QVERIFY(!resourceRequest.isEmpty());
    QVERIFY(!promptRequest.isEmpty());
    transport->fail(resourceRequest, QStringLiteral("invalid_response"),
                    QStringLiteral("Invalid resource catalog."));
    transport->respond(promptRequest,
                       {{QStringLiteral("prompts"), QJsonArray{}}});

    auto snapshot = runtime.serverSnapshot(QStringLiteral("alpha"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->state, McpServerState::Degraded);
    QCOMPARE(snapshot->lastErrorCode, QStringLiteral("invalid_response"));
    QCOMPARE(snapshot->lastErrorMessage,
             QStringLiteral("Invalid resource catalog."));

    const auto recoveryRequest = runtime.listResources(QStringLiteral("alpha"));
    QVERIFY(!recoveryRequest.isEmpty());
    transport->respond(recoveryRequest,
                       {{QStringLiteral("resources"), QJsonArray{}}});

    snapshot = runtime.serverSnapshot(QStringLiteral("alpha"));
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->state, McpServerState::Ready);
    QVERIFY(snapshot->lastErrorCode.isEmpty());
    QVERIFY(snapshot->lastErrorMessage.isEmpty());
}

void StdioMcpTransportTest::pingsBothDirections()
{
    using infrastructure::mcp::McpHostRuntime;

    QSharedPointer<FakeMcpTransport> transport;
    McpHostRuntime runtime(
        [&transport](const infrastructure::mcp::McpServerConfig&)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            transport = QSharedPointer<FakeMcpTransport>::create();
            return transport;
        });
    QString errorMessage;
    QVERIFY2(runtime.addServer(runtimeTestConfig(QStringLiteral("alpha")),
                               errorMessage),
             qPrintable(errorMessage));
    runtime.startServer(QStringLiteral("alpha"));
    const auto initializeRequest = runtime.initialize(QStringLiteral("alpha"));
    transport->respond(initializeRequest, initializeResult());

    QSignalSpy completedSpy(&runtime, &McpHostRuntime::pingCompleted);
    const auto pingRequest = runtime.ping(QStringLiteral("alpha"));
    QVERIFY(!pingRequest.isEmpty());
    QCOMPARE(transport->requestForMethod(QStringLiteral("ping")), pingRequest);
    transport->respond(pingRequest, {});
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.constFirst().at(0).toString(),
             QStringLiteral("alpha"));
    QCOMPARE(completedSpy.constFirst().at(1).toString(), pingRequest);
    QVERIFY(completedSpy.constFirst().at(2).toLongLong() >= 0);

    QSignalSpy requestedSpy(&runtime, &McpHostRuntime::pingRequested);
    transport->sendRequest(QStringLiteral("server-ping"),
                           QStringLiteral("ping"));
    QCOMPARE(requestedSpy.count(), 1);
    QCOMPARE(transport->response(QStringLiteral("server-ping")), QJsonObject{});
}

void StdioMcpTransportTest::servesOnlyAuthorizedRoots()
{
    using infrastructure::mcp::McpHostRuntime;

    QTemporaryDir authorized;
    QVERIFY(authorized.isValid());
    QSharedPointer<FakeMcpTransport> transport;
    McpHostRuntime runtime(
        [&transport](const infrastructure::mcp::McpServerConfig&)
            -> QSharedPointer<infrastructure::mcp::McpTransport>
        {
            transport = QSharedPointer<FakeMcpTransport>::create();
            return transport;
        });
    auto config = runtimeTestConfig(QStringLiteral("alpha"));
    config.authorizedRoots = {authorized.path()};
    QString errorMessage;
    QVERIFY2(runtime.addServer(config, errorMessage), qPrintable(errorMessage));
    runtime.startServer(config.serverId);
    const auto initializeRequest = runtime.initialize(config.serverId);
    transport->respond(initializeRequest, catalogInitializeResult());

    QSignalSpy rootsSpy(&runtime, &McpHostRuntime::rootsRequested);
    transport->sendRequest(QStringLiteral("roots-1"),
                           QStringLiteral("roots/list"));
    QCOMPARE(rootsSpy.count(), 1);
    const auto response = transport->response(QStringLiteral("roots-1"));
    const auto roots = response.value(QStringLiteral("roots")).toArray();
    QCOMPARE(roots.size(), 1);
    QCOMPARE(roots.at(0).toObject().value(QStringLiteral("uri")).toString(),
             runtime.roots(config.serverId).constFirst().uri);

    transport->sendRequest(QStringLiteral("sampling-1"),
                           QStringLiteral("sampling/createMessage"));
    QCOMPARE(transport->responseError(QStringLiteral("sampling-1"))
                 .value(QStringLiteral("code"))
                 .toInt(),
             -32601);

    auto invalid = runtimeTestConfig(QStringLiteral("invalid-root"));
    invalid.authorizedRoots = {
        QDir(authorized.path()).filePath(QStringLiteral("missing"))};
    QVERIFY(!runtime.addServer(invalid, errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("root"), Qt::CaseInsensitive));
}

void StdioMcpTransportTest::roundTripsRootsOverStdio()
{
    QTemporaryDir authorized;
    QVERIFY(authorized.isValid());
    auto config = testConfig();
    config.arguments.append(QStringLiteral("--request-roots"));
    config.authorizedRoots = {authorized.path()};

    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY2(manager.addServer(config, errorMessage), qPrintable(errorMessage));
    QSignalSpy notificationSpy(
        &manager, &infrastructure::mcp::McpClientManager::notificationReceived);
    manager.startServer(config.serverId);
    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    QVERIFY(!manager.initialize(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);
    QTRY_VERIFY_WITH_TIMEOUT(notificationSpy.count() >= 1, 2'000);

    auto foundRoots = false;
    for (const auto& arguments : notificationSpy)
    {
        if (arguments.at(1).toString() != QLatin1String("test/roots_received"))
            continue;
        const auto roots = arguments.at(2)
                               .toJsonObject()
                               .value(QStringLiteral("roots"))
                               .toArray();
        QCOMPARE(roots.size(), 1);
        QCOMPARE(roots.at(0).toObject().value(QStringLiteral("uri")).toString(),
                 manager.roots(config.serverId).constFirst().uri);
        foundRoots = true;
    }
    QVERIFY(foundRoots);
}

void StdioMcpTransportTest::roundTripsPingOverStdio()
{
    auto config = testConfig();
    config.arguments.append(QStringLiteral("--request-ping"));

    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY2(manager.addServer(config, errorMessage), qPrintable(errorMessage));
    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    QSignalSpy requestedSpy(
        &manager, &infrastructure::mcp::McpClientManager::pingRequested);
    QSignalSpy notificationSpy(
        &manager, &infrastructure::mcp::McpClientManager::notificationReceived);
    QSignalSpy completedSpy(
        &manager, &infrastructure::mcp::McpClientManager::pingCompleted);

    manager.startServer(config.serverId);
    QVERIFY(!manager.initialize(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);
    QTRY_COMPARE_WITH_TIMEOUT(requestedSpy.count(), 1, 2'000);
    QTRY_VERIFY_WITH_TIMEOUT(notificationSpy.count() >= 1, 2'000);

    auto serverReceivedPing = false;
    for (const auto& arguments : notificationSpy)
        if (arguments.at(1).toString() == QLatin1String("test/ping_received"))
            serverReceivedPing = true;
    QVERIFY(serverReceivedPing);

    const auto requestId = manager.ping(config.serverId);
    QVERIFY(!requestId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2'000);
    QCOMPARE(completedSpy.constFirst().at(0).toString(), config.serverId);
    QCOMPARE(completedSpy.constFirst().at(1).toString(), requestId);
}

void StdioMcpTransportTest::roundTripsCompletionAndLoggingOverStdio()
{
    auto config = testConfig();
    infrastructure::mcp::McpClientManager manager;
    QString errorMessage;
    QVERIFY2(manager.addServer(config, errorMessage), qPrintable(errorMessage));
    QSignalSpy initializedSpy(
        &manager, &infrastructure::mcp::McpClientManager::serverInitialized);
    QSignalSpy promptsSpy(
        &manager, &infrastructure::mcp::McpClientManager::promptsChanged);
    QSignalSpy templatesSpy(
        &manager,
        &infrastructure::mcp::McpClientManager::resourceTemplatesChanged);
    QSignalSpy completionSpy(
        &manager, &infrastructure::mcp::McpClientManager::completionReady);
    QSignalSpy levelSpy(
        &manager, &infrastructure::mcp::McpClientManager::loggingLevelChanged);

    manager.startServer(config.serverId);
    QVERIFY(!manager.initialize(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(initializedSpy.count(), 1, 2'000);
    QVERIFY(!manager.listPrompts(config.serverId).isEmpty());
    QVERIFY(!manager.listResourceTemplates(config.serverId).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(promptsSpy.count(), 1, 2'000);
    QTRY_COMPARE_WITH_TIMEOUT(templatesSpy.count(), 1, 2'000);

    const auto levelRequest =
        manager.setLoggingLevel(config.serverId, QStringLiteral("warning"));
    QVERIFY(!levelRequest.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(levelSpy.count(), 1, 2'000);
    QCOMPARE(levelSpy.constFirst().at(1).toString(), QStringLiteral("warning"));

    const auto completionRequest =
        manager.completePrompt(config.serverId, QStringLiteral("summarize"),
                               QStringLiteral("topic"), QStringLiteral("mcp"));
    QVERIFY(!completionRequest.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(completionSpy.count(), 1, 2'000);
    const auto result = qvariant_cast<infrastructure::mcp::McpCompletionResult>(
        completionSpy.constFirst().constFirst());
    QCOMPARE(result.requestId, completionRequest);
    QCOMPARE(result.values, QStringList({QStringLiteral("mcp-one"),
                                         QStringLiteral("mcp-two")}));
    QVERIFY(result.totalProvided);
    QVERIFY(result.hasMoreProvided);
    QVERIFY(!result.hasMore);
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
