#include "AgentController.hpp"
#include "AgentEvent.hpp"
#include "AgentRunMetrics.hpp"
#include "ComputeDevice.hpp"
#include "McpClientManager.hpp"
#include "McpServerProcess.hpp"
#include "ModelPackage.hpp"
#include "ToolPolicy.hpp"
#include "WorkerClient.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include <cstdio>
#include <optional>
#include <utility>

namespace qtllm::tests
{
namespace
{
struct EvaluationScenario
{
    QString name;
    QString prompt;
    QString approval = QStringLiteral("allow");
    QJsonObject expectations;
};

struct EvaluationOptions
{
    QString suitePath;
    QString mcpConfigPath;
    QString outputPath;
    QString workerPath;
    QString workspaceRoot;
    int repeat = 1;
    int startupTimeoutMs = 60'000;
    int runTimeoutMs = 300'000;
    inference::ModelLoadOptions loadOptions;
};

struct EvaluationInput
{
    models::ModelSelection model;
    QList<infrastructure::mcp::McpServerConfig> servers;
    QList<EvaluationScenario> scenarios;
    EvaluationOptions options;
};

bool readJson(const QString& path, QJsonDocument& document,
              QString& errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        errorMessage =
            QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
    {
        errorMessage = QStringLiteral("Invalid JSON in %1 at offset %2: %3")
                           .arg(path)
                           .arg(parseError.offset)
                           .arg(parseError.errorString());
        return false;
    }
    return true;
}

bool parseScenarios(const QString& path, QList<EvaluationScenario>& scenarios,
                    QString& errorMessage)
{
    QJsonDocument document;
    if (!readJson(path, document, errorMessage) || !document.isObject())
    {
        if (errorMessage.isEmpty())
            errorMessage =
                QStringLiteral("Evaluation suite must be an object.");
        return false;
    }
    const auto root = document.object();
    if (root.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
        !root.value(QStringLiteral("scenarios")).isArray())
    {
        errorMessage = QStringLiteral(
            "Evaluation suite requires schemaVersion 1 and scenarios array.");
        return false;
    }
    static const QSet<QString> approvalModes{
        QStringLiteral("allow"), QStringLiteral("deny"),
        QStringLiteral("approve"), QStringLiteral("reject"),
        QStringLiteral("policy")};
    QSet<QString> names;
    for (const auto& value : root.value(QStringLiteral("scenarios")).toArray())
    {
        if (!value.isObject())
        {
            errorMessage =
                QStringLiteral("Every evaluation scenario must be an object.");
            return false;
        }
        const auto object = value.toObject();
        EvaluationScenario scenario;
        scenario.name =
            object.value(QStringLiteral("name")).toString().trimmed();
        scenario.prompt =
            object.value(QStringLiteral("prompt")).toString().trimmed();
        if (object.value(QStringLiteral("approval")).isString())
            scenario.approval =
                object.value(QStringLiteral("approval")).toString().trimmed();
        if (scenario.name.isEmpty() || scenario.prompt.isEmpty() ||
            names.contains(scenario.name) ||
            !approvalModes.contains(scenario.approval))
        {
            errorMessage = QStringLiteral(
                "Scenario names and prompts must be non-empty, names must be "
                "unique, and approval must be allow, deny, approve, reject, "
                "or policy.");
            return false;
        }
        const auto expect = object.value(QStringLiteral("expect"));
        if (!expect.isUndefined() && !expect.isObject())
        {
            errorMessage =
                QStringLiteral("Scenario %1 expect must be an object.")
                    .arg(scenario.name);
            return false;
        }
        scenario.expectations = expect.toObject();
        for (const auto& group :
             {QStringLiteral("equals"), QStringLiteral("maximum"),
              QStringLiteral("minimum")})
        {
            const auto groupValue = scenario.expectations.value(group);
            if (!groupValue.isUndefined() && !groupValue.isObject())
            {
                errorMessage =
                    QStringLiteral("Scenario %1 expect.%2 must be an object.")
                        .arg(scenario.name, group);
                return false;
            }
            if (group == QLatin1String("equals")) continue;
            const auto groupObject = groupValue.toObject();
            for (auto item = groupObject.constBegin();
                 item != groupObject.constEnd(); ++item)
            {
                if (!item->isDouble())
                {
                    errorMessage =
                        QStringLiteral(
                            "Scenario %1 expect.%2.%3 must be numeric.")
                            .arg(scenario.name, group, item.key());
                    return false;
                }
            }
        }
        names.insert(scenario.name);
        scenarios.append(std::move(scenario));
    }
    if (scenarios.isEmpty())
    {
        errorMessage = QStringLiteral("Evaluation suite has no scenarios.");
        return false;
    }
    return true;
}

bool parseServerConfigs(
    const QString& path,
    QList<infrastructure::mcp::McpServerConfig>& configurations,
    QString& errorMessage)
{
    QJsonDocument document;
    if (!readJson(path, document, errorMessage) || !document.isArray())
    {
        if (errorMessage.isEmpty())
            errorMessage =
                QStringLiteral("MCP configuration must be an array.");
        return false;
    }
    const auto configDirectory = QFileInfo(path).absoluteDir();
    for (const auto& value : document.array())
    {
        if (!value.isObject())
        {
            errorMessage = QStringLiteral(
                "Every MCP server configuration must be an object.");
            return false;
        }
        infrastructure::mcp::McpServerConfig config;
        if (!infrastructure::mcp::parseServerConfig(value.toObject(), config,
                                                    errorMessage))
            return false;
        if (QFileInfo(config.program).isRelative())
            config.program = configDirectory.absoluteFilePath(config.program);
        if (!config.workingDirectory.isEmpty() &&
            QFileInfo(config.workingDirectory).isRelative())
            config.workingDirectory =
                configDirectory.absoluteFilePath(config.workingDirectory);
        if (config.enabled) configurations.append(std::move(config));
    }
    if (configurations.isEmpty())
    {
        errorMessage =
            QStringLiteral("MCP configuration has no enabled servers.");
        return false;
    }
    return true;
}

QJsonObject presetJson(const models::InferencePreset& preset)
{
    return {{QStringLiteral("contextSize"), preset.contextSize},
            {QStringLiteral("maxOutputTokens"), preset.maxOutputTokens},
            {QStringLiteral("temperature"), preset.temperature},
            {QStringLiteral("topP"), preset.topP},
            {QStringLiteral("topK"), preset.topK},
            {QStringLiteral("repeatPenalty"), preset.repeatPenalty}};
}

QJsonObject modelJson(const models::ModelSelection& model)
{
    return {
        {QStringLiteral("id"), model.descriptor.id},
        {QStringLiteral("displayName"), model.descriptor.displayName},
        {QStringLiteral("selectedPath"), model.selectedPath},
        {QStringLiteral("modelPath"), model.modelPath},
        {QStringLiteral("modelFile"), QFileInfo(model.modelPath).fileName()},
        {QStringLiteral("packageVersion"), model.descriptor.packageVersion}};
}

QJsonObject eventJson(const agent::Event& event)
{
    QJsonObject result{
        {QStringLiteral("type"), agent::eventTypeName(event.type)},
        {QStringLiteral("timestamp"),
         event.timestamp.toString(Qt::ISODateWithMs)},
        {QStringLiteral("message"), event.message}};
    if (!event.toolName.isEmpty())
        result.insert(QStringLiteral("tool"), event.toolName);
    if (!event.data.isEmpty())
        result.insert(QStringLiteral("data"), event.data);
    return result;
}

QStringList evaluateExpectations(const QJsonObject& metrics,
                                 const QJsonObject& expectations)
{
    QStringList failures;
    const auto equals = expectations.value(QStringLiteral("equals")).toObject();
    for (auto item = equals.constBegin(); item != equals.constEnd(); ++item)
    {
        if (metrics.value(item.key()) != item.value())
            failures.append(
                QStringLiteral("%1 expected %2 but was %3")
                    .arg(
                        item.key(),
                        QString::fromUtf8(
                            QJsonDocument(QJsonArray{item.value()})
                                .toJson(QJsonDocument::Compact)),
                        QString::fromUtf8(
                            QJsonDocument(QJsonArray{metrics.value(item.key())})
                                .toJson(QJsonDocument::Compact))));
    }
    const auto compareNumbers =
        [&metrics, &failures](const QJsonObject& expected, bool maximum)
    {
        for (auto item = expected.constBegin(); item != expected.constEnd();
             ++item)
        {
            const auto actual = metrics.value(item.key());
            if (!actual.isDouble() ||
                (maximum && actual.toDouble() > item->toDouble()) ||
                (!maximum && actual.toDouble() < item->toDouble()))
                failures.append(
                    QStringLiteral("%1 expected %2 %3 but was %4")
                        .arg(item.key(), maximum ? QStringLiteral("at most")
                                                 : QStringLiteral("at least"))
                        .arg(item->toDouble())
                        .arg(actual.isDouble()
                                 ? QString::number(actual.toDouble())
                                 : QStringLiteral("non-numeric")));
        }
    };
    compareNumbers(expectations.value(QStringLiteral("maximum")).toObject(),
                   true);
    compareNumbers(expectations.value(QStringLiteral("minimum")).toObject(),
                   false);
    return failures;
}

class EvaluationRunner final : public QObject
{
   public:
    EvaluationRunner(EvaluationInput input, QCoreApplication& app)
        : QObject(&app),
          input_(std::move(input)),
          application_(app),
          mcpManager_(this),
          worker_(this),
          controller_(
              application::AgentController::Dependencies{
                  [this](const QList<chat::Message>& messages,
                         const models::InferencePreset& preset, int maxTokens)
                  {
                      worker_.generate(messages, preset.contextSize, maxTokens,
                                       0, preset.temperature, preset.topP,
                                       preset.topK, preset.repeatPenalty,
                                       inference::ResponseMode::AgentAction);
                  },
                  [this] { worker_.cancel(); },
                  [this](const QString& toolName, const QJsonObject& arguments)
                  { return mcpManager_.callTool(toolName, arguments); },
                  [this](const QString& requestId)
                  { mcpManager_.cancel(requestId); },
                  [this](const QString& toolName, const QJsonObject& arguments)
                  {
                      return mcpManager_.registry().validateArgumentsDetailed(
                          toolName, arguments);
                  },
                  [this](const QString& toolName)
                  { return toolDecision(toolName); },
                  [this](const QString& toolName)
                  { return toolPolicy_.risk(toolName); }},
              this),
          startupTimer_(this),
          runTimer_(this)
    {
        report_.insert(QStringLiteral("schemaVersion"), 1);
        report_.insert(
            QStringLiteral("startedAt"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        report_.insert(QStringLiteral("suite"), input_.options.suitePath);
        report_.insert(QStringLiteral("mcpConfig"),
                       input_.options.mcpConfigPath);
        report_.insert(QStringLiteral("model"), modelJson(input_.model));
        report_.insert(QStringLiteral("preset"),
                       presetJson(input_.model.preset));
        report_.insert(QStringLiteral("repeat"), input_.options.repeat);
        report_.insert(QStringLiteral("workspaceRoot"),
                       input_.options.workspaceRoot);

        startupTimer_.setSingleShot(true);
        runTimer_.setSingleShot(true);
        connect(&startupTimer_, &QTimer::timeout, this,
                [this]
                {
                    failFatal(
                        QStringLiteral("startup_timeout"),
                        QStringLiteral("Worker or MCP startup timed out."));
                });
        connect(&runTimer_, &QTimer::timeout, this,
                [this]
                {
                    currentHostDiagnostics_.append(QJsonObject{
                        {QStringLiteral("code"), QStringLiteral("run_timeout")},
                        {QStringLiteral("message"),
                         QStringLiteral("Scenario exceeded its "
                                        "configured timeout.")}});
                    controller_.cancel();
                });
        connectRuntime();
    }

    void start()
    {
        startupTimer_.start(input_.options.startupTimeoutMs);
        for (const auto& config : input_.servers)
        {
            QString errorMessage;
            if (!mcpManager_.addServer(config, errorMessage))
            {
                failFatal(QStringLiteral("mcp_config_failed"), errorMessage);
                return;
            }
            pendingServers_.insert(config.serverId);
            mcpManager_.startServer(config.serverId);
        }
        worker_.start(input_.options.workerPath);
    }

   private:
    void connectRuntime()
    {
        connect(&worker_, &infrastructure::WorkerClient::stateChanged, this,
                [this](infrastructure::WorkerClient::State state)
                {
                    if (state == infrastructure::WorkerClient::State::Ready &&
                        !modelLoadStarted_)
                    {
                        modelLoadStarted_ = true;
                        worker_.loadModel(input_.model.modelPath,
                                          input_.options.loadOptions);
                    }
                });
        connect(
            &worker_, &infrastructure::WorkerClient::modelLoaded, this,
            [this](const QString&, qint64 milliseconds, const QString& device)
            {
                modelReady_ = true;
                modelLoad_ = {
                    {QStringLiteral("milliseconds"), milliseconds},
                    {QStringLiteral("device"), device},
                    {QStringLiteral("splitMode"), worker_.activeSplitMode()},
                    {QStringLiteral("gpuLayers"),
                     input_.options.loadOptions.gpuLayers}};
                startRunsIfReady();
            });
        connect(&worker_, &infrastructure::WorkerClient::modelLoadFailed, this,
                [this](const QString& code, const QString& message)
                { failFatal(code, message); });
        connect(&worker_, &infrastructure::WorkerClient::tokenReceived,
                &controller_, &application::AgentController::receiveToken);
        connect(&worker_, &infrastructure::WorkerClient::generationFinished,
                this,
                [this](bool cancelled, const QJsonObject& metrics)
                {
                    if (controller_.hasActiveRun())
                        currentInferenceMetrics_.append(metrics);
                    controller_.completeGeneration(cancelled, metrics);
                });
        connect(&worker_, &infrastructure::WorkerClient::errorOccurred, this,
                [this](const QString& code, const QString& message)
                {
                    if (controller_.hasActiveRun())
                        controller_.handleGenerationError(code, message);
                    else if (!modelReady_)
                        failFatal(code, message);
                });

        connect(&mcpManager_,
                &infrastructure::mcp::McpClientManager::serverStarted, this,
                [this](const QString& serverId)
                { mcpManager_.initialize(serverId); });
        connect(&mcpManager_,
                &infrastructure::mcp::McpClientManager::serverInitialized, this,
                [this](const QString& serverId, const QJsonObject&)
                {
                    const auto snapshot = mcpManager_.serverSnapshot(serverId);
                    if (snapshot && snapshot->capabilities.tools)
                        mcpManager_.listTools(serverId);
                    else
                        markServerReady(serverId);
                });
        connect(&mcpManager_,
                &infrastructure::mcp::McpClientManager::toolsChanged, this,
                [this](const QString& serverId,
                       const QList<agent::ToolDefinition>& tools)
                {
                    for (const auto& tool : tools)
                        toolPolicy_.setRule(
                            tool.qualifiedName,
                            infrastructure::mcp::defaultToolPolicyRule(
                                tool.qualifiedName));
                    markServerReady(serverId);
                });
        connect(&mcpManager_,
                &infrastructure::mcp::McpClientManager::toolResultReady,
                &controller_, &application::AgentController::receiveToolResult);
        connect(
            &mcpManager_, &infrastructure::mcp::McpClientManager::requestFailed,
            this,
            [this](const QString& serverId, const QString& requestId,
                   const QString& method, const QString& code,
                   const QString& message)
            {
                currentHostDiagnostics_.append(
                    QJsonObject{{QStringLiteral("server"), serverId},
                                {QStringLiteral("requestId"), requestId},
                                {QStringLiteral("method"), method},
                                {QStringLiteral("code"), code},
                                {QStringLiteral("message"), message}});
                if (!runsStarted_ && (method == QLatin1String("initialize") ||
                                      method == QLatin1String("tools/list")))
                    failFatal(code, message);
            });
        connect(&mcpManager_,
                &infrastructure::mcp::McpClientManager::serverError, this,
                [this](const QString& serverId, const QString& code,
                       const QString& message)
                {
                    currentHostDiagnostics_.append(
                        QJsonObject{{QStringLiteral("server"), serverId},
                                    {QStringLiteral("code"), code},
                                    {QStringLiteral("message"), message}});
                    if (!runsStarted_) failFatal(code, message);
                });

        connect(&controller_, &application::AgentController::approvalRequested,
                this,
                [this](const QString&, const QString&, const QJsonObject&)
                {
                    controller_.resolveApproval(currentScenario().approval !=
                                                QLatin1String("reject"));
                });
        connect(&controller_, &application::AgentController::metricsReady, this,
                [this](const QString&, const QJsonObject& metrics)
                { currentMetrics_ = metrics; });
        connect(&controller_, &application::AgentController::runFinished, this,
                [this](const QString&, application::AgentRun::State,
                       const QString&, const QString&) { finishRun(); });
    }

    infrastructure::mcp::ToolDecision toolDecision(
        const QString& toolName) const
    {
        const auto approval = currentScenario().approval;
        if (approval == QLatin1String("allow"))
            return infrastructure::mcp::ToolDecision::Allow;
        if (approval == QLatin1String("deny"))
            return infrastructure::mcp::ToolDecision::Deny;
        if (approval == QLatin1String("approve") ||
            approval == QLatin1String("reject"))
            return infrastructure::mcp::ToolDecision::RequireApproval;
        return toolPolicy_.evaluate(toolName);
    }

    const EvaluationScenario& currentScenario() const
    {
        return input_.scenarios.at(scenarioIndex_);
    }

    void markServerReady(const QString& serverId)
    {
        pendingServers_.remove(serverId);
        startRunsIfReady();
    }

    void startRunsIfReady()
    {
        if (fatal_ || runsStarted_ || !modelReady_ ||
            !pendingServers_.isEmpty())
            return;
        if (mcpManager_.tools().isEmpty())
        {
            failFatal(QStringLiteral("agent_tools_unavailable"),
                      QStringLiteral("MCP servers published no Agent tools."));
            return;
        }
        runsStarted_ = true;
        startupTimer_.stop();
        QTimer::singleShot(0, this, [this] { startNextRun(); });
    }

    void startNextRun()
    {
        if (fatal_) return;
        if (scenarioIndex_ >= input_.scenarios.size())
        {
            finishEvaluation();
            return;
        }
        currentMetrics_ = {};
        currentInferenceMetrics_ = {};
        currentHostDiagnostics_ = {};
        controller_.clearConversation();
        runElapsed_.restart();
        runTimer_.start(input_.options.runTimeoutMs);
        const application::AssistantContext context{
            input_.model.descriptor.displayName, input_.options.workspaceRoot,
            mcpManager_.agentInstructions()};
        if (!controller_.start(currentScenario().prompt, input_.model.preset,
                               mcpManager_.tools(), context))
        {
            failFatal(QStringLiteral("agent_start_failed"),
                      QStringLiteral("AgentController rejected the scenario."));
        }
    }

    void finishRun()
    {
        if (fatal_) return;
        runTimer_.stop();
        if (currentMetrics_.isEmpty() && controller_.activeRun())
            currentMetrics_ =
                application::AgentRunMetrics::fromRun(*controller_.activeRun())
                    .toJson();
        QJsonArray trace;
        if (controller_.activeRun())
            for (const auto& event : controller_.activeRun()->events)
                trace.append(eventJson(event));
        const auto failures = evaluateExpectations(
            currentMetrics_, currentScenario().expectations);
        QJsonArray failureJson;
        for (const auto& failure : failures)
            failureJson.append(failure);
        QJsonObject run{
            {QStringLiteral("scenario"), currentScenario().name},
            {QStringLiteral("iteration"), iteration_ + 1},
            {QStringLiteral("elapsedMs"), runElapsed_.elapsed()},
            {QStringLiteral("accepted"), failures.isEmpty()},
            {QStringLiteral("acceptanceFailures"), failureJson},
            {QStringLiteral("metrics"), currentMetrics_},
            {QStringLiteral("inferenceMetrics"), currentInferenceMetrics_},
            {QStringLiteral("hostDiagnostics"), currentHostDiagnostics_},
            {QStringLiteral("trace"), trace}};
        runs_.append(run);
        if (failures.isEmpty())
            ++acceptedRuns_;
        else
            ++rejectedRuns_;

        ++iteration_;
        if (iteration_ >= input_.options.repeat)
        {
            iteration_ = 0;
            ++scenarioIndex_;
        }
        QTimer::singleShot(0, this, [this] { startNextRun(); });
    }

    void failFatal(const QString& code, const QString& message)
    {
        if (fatal_) return;
        fatal_ = true;
        startupTimer_.stop();
        runTimer_.stop();
        report_.insert(QStringLiteral("fatalError"),
                       QJsonObject{{QStringLiteral("code"), code},
                                   {QStringLiteral("message"), message}});
        finishEvaluation(2);
    }

    void finishEvaluation(int forcedExitCode = -1)
    {
        if (finished_) return;
        finished_ = true;
        report_.insert(
            QStringLiteral("finishedAt"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        report_.insert(QStringLiteral("modelLoad"), modelLoad_);
        QJsonArray servers;
        for (const auto& snapshot : mcpManager_.serverSnapshots())
            servers.append(QJsonObject{
                {QStringLiteral("serverId"), snapshot.serverId},
                {QStringLiteral("protocolVersion"), snapshot.protocolVersion},
                {QStringLiteral("serverInfo"), snapshot.serverInfo},
                {QStringLiteral("toolCount"), snapshot.toolCount}});
        report_.insert(QStringLiteral("servers"), servers);
        if (!runsStarted_ && !currentHostDiagnostics_.isEmpty())
            report_.insert(QStringLiteral("startupDiagnostics"),
                           currentHostDiagnostics_);
        report_.insert(QStringLiteral("runs"), runs_);
        report_.insert(
            QStringLiteral("summary"),
            QJsonObject{{QStringLiteral("total"), runs_.size()},
                        {QStringLiteral("accepted"), acceptedRuns_},
                        {QStringLiteral("rejected"), rejectedRuns_}});
        auto exitCode =
            forcedExitCode >= 0 ? forcedExitCode : (rejectedRuns_ == 0 ? 0 : 1);
        QString writeError;
        if (!writeReport(writeError))
        {
            QTextStream(stderr) << writeError << Qt::endl;
            exitCode = 2;
        }
        for (const auto& serverId : mcpManager_.serverIds())
            mcpManager_.stopServer(serverId);
        worker_.stop();
        application_.exit(exitCode);
    }

    bool writeReport(QString& errorMessage) const
    {
        const auto serialized =
            QJsonDocument(report_).toJson(QJsonDocument::Indented);
        if (input_.options.outputPath.isEmpty())
        {
            QTextStream(stdout) << serialized;
            return true;
        }
        QSaveFile file(input_.options.outputPath);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(serialized) != serialized.size() || !file.commit())
        {
            errorMessage =
                QStringLiteral("Cannot write report %1: %2")
                    .arg(input_.options.outputPath, file.errorString());
            return false;
        }
        return true;
    }

    EvaluationInput input_;
    QCoreApplication& application_;
    infrastructure::mcp::McpClientManager mcpManager_;
    infrastructure::WorkerClient worker_;
    infrastructure::mcp::ToolPolicy toolPolicy_;
    application::AgentController controller_;
    QTimer startupTimer_;
    QTimer runTimer_;
    QElapsedTimer runElapsed_;
    QSet<QString> pendingServers_;
    QJsonObject report_;
    QJsonObject modelLoad_;
    QJsonObject currentMetrics_;
    QJsonArray currentInferenceMetrics_;
    QJsonArray currentHostDiagnostics_;
    QJsonArray runs_;
    int scenarioIndex_ = 0;
    int iteration_ = 0;
    int acceptedRuns_ = 0;
    int rejectedRuns_ = 0;
    bool modelLoadStarted_ = false;
    bool modelReady_ = false;
    bool runsStarted_ = false;
    bool fatal_ = false;
    bool finished_ = false;
};

bool positiveInteger(const QString& value, int& parsed)
{
    bool ok = false;
    parsed = value.toInt(&ok);
    return ok && parsed > 0;
}

std::optional<EvaluationInput> parseInput(QCoreApplication& application,
                                          QString& errorMessage)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Run headless qtLLM Agent acceptance scenarios."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption modelOption(
        QStringLiteral("model"), QStringLiteral("GGUF file or model package."),
        QStringLiteral("path"));
    const QCommandLineOption suiteOption(
        QStringLiteral("suite"), QStringLiteral("Evaluation suite JSON."),
        QStringLiteral("path"));
    const QCommandLineOption mcpConfigOption(
        QStringLiteral("mcp-config"),
        QStringLiteral("JSON array of standard qtLLM MCP server configs."),
        QStringLiteral("path"));
    const QCommandLineOption outputOption(
        QStringLiteral("output"),
        QStringLiteral("Report path; stdout when omitted."),
        QStringLiteral("path"));
    const QCommandLineOption workerOption(
        QStringLiteral("worker"),
        QStringLiteral("qtllm-worker path; auto-detected when omitted."),
        QStringLiteral("path"));
    const QCommandLineOption workspaceOption(
        QStringLiteral("workspace"),
        QStringLiteral("Workspace root included in the Agent context."),
        QStringLiteral("path"), QDir::currentPath());
    const QCommandLineOption repeatOption(
        QStringLiteral("repeat"), QStringLiteral("Runs per scenario."),
        QStringLiteral("count"), QStringLiteral("1"));
    const QCommandLineOption startupTimeoutOption(
        QStringLiteral("startup-timeout-ms"),
        QStringLiteral("Worker and MCP startup timeout."),
        QStringLiteral("milliseconds"), QStringLiteral("60000"));
    const QCommandLineOption runTimeoutOption(
        QStringLiteral("run-timeout-ms"),
        QStringLiteral("Timeout for each scenario run."),
        QStringLiteral("milliseconds"), QStringLiteral("300000"));
    const QCommandLineOption gpuLayersOption(
        QStringLiteral("gpu-layers"),
        QStringLiteral("GPU layers passed to the worker; -1 is automatic."),
        QStringLiteral("count"), QStringLiteral("-1"));
    parser.addOptions({modelOption, suiteOption, mcpConfigOption, outputOption,
                       workerOption, workspaceOption, repeatOption,
                       startupTimeoutOption, runTimeoutOption,
                       gpuLayersOption});
    parser.process(application);
    if (!parser.isSet(modelOption) || !parser.isSet(suiteOption) ||
        !parser.isSet(mcpConfigOption))
    {
        errorMessage =
            QStringLiteral("--model, --suite, and --mcp-config are required.");
        return std::nullopt;
    }

    EvaluationInput input;
    input.options.suitePath =
        QFileInfo(parser.value(suiteOption)).absoluteFilePath();
    input.options.mcpConfigPath =
        QFileInfo(parser.value(mcpConfigOption)).absoluteFilePath();
    input.options.outputPath = parser.value(outputOption);
    input.options.workerPath = parser.value(workerOption);
    const QFileInfo workspaceInfo(parser.value(workspaceOption));
    input.options.workspaceRoot = workspaceInfo.canonicalFilePath();
    if (!workspaceInfo.isDir() || input.options.workspaceRoot.isEmpty())
    {
        errorMessage =
            QStringLiteral("--workspace must name an existing directory.");
        return std::nullopt;
    }
    if (!positiveInteger(parser.value(repeatOption), input.options.repeat) ||
        !positiveInteger(parser.value(startupTimeoutOption),
                         input.options.startupTimeoutMs) ||
        !positiveInteger(parser.value(runTimeoutOption),
                         input.options.runTimeoutMs))
    {
        errorMessage = QStringLiteral(
            "Repeat and timeout values must be positive integers.");
        return std::nullopt;
    }
    bool gpuLayersOk = false;
    input.options.loadOptions.gpuLayers =
        parser.value(gpuLayersOption).toInt(&gpuLayersOk);
    if (!gpuLayersOk || input.options.loadOptions.gpuLayers < -1)
    {
        errorMessage = QStringLiteral("--gpu-layers must be -1 or greater.");
        return std::nullopt;
    }
    if (!parseScenarios(input.options.suitePath, input.scenarios,
                        errorMessage) ||
        !parseServerConfigs(input.options.mcpConfigPath, input.servers,
                            errorMessage))
        return std::nullopt;

    auto modelResult = models::ModelPackage::inspect(
        parser.value(modelOption), application.applicationVersion());
    if (!modelResult.succeeded())
    {
        errorMessage = modelResult.errorMessage;
        return std::nullopt;
    }
    modelResult =
        models::ModelPackage::verify(std::move(modelResult.selection));
    if (!modelResult.succeeded())
    {
        errorMessage = modelResult.errorMessage;
        return std::nullopt;
    }
    input.model = std::move(modelResult.selection);
    return input;
}
}  // namespace
}  // namespace qtllm::tests

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qtllm-agent-eval"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    QString errorMessage;
    auto input = qtllm::tests::parseInput(application, errorMessage);
    if (!input)
    {
        QTextStream(stderr) << "error: " << errorMessage << Qt::endl;
        return 2;
    }
    qtllm::tests::EvaluationRunner runner(std::move(*input), application);
    QTimer::singleShot(0, &runner, [&runner] { runner.start(); });
    return application.exec();
}
