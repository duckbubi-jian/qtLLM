#include "MainWindow.hpp"

#include "BuiltInMcpServer.hpp"
#include "ChatView.hpp"
#include "McpControlPanel.hpp"
#include "McpServerDialog.hpp"
#include "MessageWidget.hpp"
#include "SensitiveData.hpp"
#include "ToolApprovalWidget.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QStringList>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace qtllm::ui
{
namespace
{
ModelBadgeState modelBadgeState(const models::ModelSelection& selection)
{
    if (selection.selectedPath.isEmpty()) return ModelBadgeState::Neutral;
    if (selection.descriptor.verificationStatus ==
        models::VerificationStatus::Invalid)
        return ModelBadgeState::Invalid;
    if (selection.descriptor.origin == models::ModelOrigin::DirectGguf)
        return ModelBadgeState::Unverified;
    return selection.descriptor.verificationStatus ==
                   models::VerificationStatus::Verified
               ? ModelBadgeState::Verified
               : ModelBadgeState::Unverified;
}
}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      mcpManager_(this),
      chatController_(
          [this](const QList<chat::Message>& messages,
                 const models::InferencePreset& preset)
          {
              workerClient_.generate(
                  messages, preset.contextSize, preset.maxOutputTokens, 0,
                  preset.temperature, preset.topP, preset.topK,
                  preset.repeatPenalty, inference::ResponseMode::Text);
          },
          [this] { workerClient_.cancel(); }, this),
      agentController_(
          application::AgentController::Dependencies{
              [this](const QList<chat::Message>& messages,
                     const models::InferencePreset& preset, int maxTokens)
              {
                  workerClient_.generate(messages, preset.contextSize,
                                         maxTokens, 0, preset.temperature,
                                         preset.topP, preset.topK,
                                         preset.repeatPenalty,
                                         inference::ResponseMode::AgentAction);
              },
              [this] { workerClient_.cancel(); },
              [this](const QString& toolName, const QJsonObject& arguments)
              { return mcpManager_.callTool(toolName, arguments); },
              [this](const QString& requestId)
              { mcpManager_.cancel(requestId); },
              [this](const QString& toolName, const QJsonObject& arguments,
                     QString& errorMessage)
              {
                  return mcpManager_.registry().validateArguments(
                      toolName, arguments, errorMessage);
              },
              [this](const QString& toolName)
              { return toolPolicy_.evaluate(toolName); }},
          this)
{
    resize(1080, 760);
    setMinimumSize(720, 520);

    buildUi();
    chatView_->setAgentModeSelected(settingsStore_.agentModeEnabled());
    modelInfoText_ = tr("Model not checked");
    setModelPath(settingsStore_.lastModelPath());
    auto workspacePath = settingsStore_.workspacePath();
    QFileInfo workspaceInfo(workspacePath);
    if (!workspaceInfo.isDir() || workspaceInfo.canonicalFilePath().isEmpty())
    {
        workspacePath = QFileInfo(QDir::homePath()).canonicalFilePath();
    }
    else
    {
        workspacePath = workspaceInfo.canonicalFilePath();
    }
    workspacePath_ = workspacePath;
    chatView_->setWorkspacePresentation(workspacePath_);

    connect(&workerClient_, &infrastructure::WorkerClient::stateChanged, this,
            &MainWindow::updateState);
    connect(
        &workerClient_, &infrastructure::WorkerClient::modelLoaded, this,
        [this](const QString& path, qint64 milliseconds, const QString& device)
        {
            Q_UNUSED(path)
            replacingModel_ = false;
            activeModelSelection_ = pendingModelSelection_;
            chatView_->setStatusText(
                tr("Model ready on %1 - loaded in %2 ms")
                    .arg(device.isEmpty() ? tr("CPU") : device)
                    .arg(milliseconds));
            modelPath_ = activeModelSelection_.selectedPath;
            updateModelInformation(activeModelSelection_);
            if (!settingsStore_.setLastModelPath(
                    activeModelSelection_.selectedPath))
            {
                chatView_->setStatusText(
                    tr("Model ready, but the model path could not be "
                       "saved beside the application."));
            }
        });
    connect(&workerClient_, &infrastructure::WorkerClient::modelUnloaded, this,
            &MainWindow::continueModelLoadAfterUnload);
    connect(&workerClient_, &infrastructure::WorkerClient::modelLoadFailed,
            this, &MainWindow::handleModelLoadFailure);
    connect(&workerClient_, &infrastructure::WorkerClient::modelUnloadFailed,
            this, &MainWindow::handleModelUnloadFailure);
    connect(&modelVerificationWatcher_,
            &QFutureWatcher<models::ModelPackageResult>::finished, this,
            &MainWindow::finishModelPackageVerification);
    connect(&workerClient_, &infrastructure::WorkerClient::tokenReceived, this,
            [this](const QByteArray& bytes)
            {
                chatController_.receiveToken(bytes);
                agentController_.receiveToken(bytes);
            });
    connect(&workerClient_, &infrastructure::WorkerClient::generationFinished,
            this,
            [this](bool cancelled, const QJsonObject& metrics)
            {
                chatController_.completeGeneration(cancelled, metrics);
                agentController_.completeGeneration(cancelled, metrics);
            });
    connect(&workerClient_, &infrastructure::WorkerClient::errorOccurred, this,
            [this](const QString& code, const QString& message)
            {
                chatController_.handleError(code, message);
                agentController_.handleGenerationError(code, message);
            });
    connect(&chatController_, &application::ChatController::userMessageAccepted,
            this,
            [this](const QString& prompt)
            {
                appendUserMessage(prompt);
                chatView_->promptEditor()->clear();
                pendingUtf8_.clear();
                currentAssistantText_.clear();
            });
    connect(&chatController_,
            &application::ChatController::assistantResponseStarted, this,
            &MainWindow::beginAssistantMessage);
    connect(&chatController_, &application::ChatController::tokenReceived, this,
            &MainWindow::appendToken);
    connect(&chatController_, &application::ChatController::generationFinished,
            this,
            [this](bool cancelled, const QJsonObject& metrics)
            {
                conversationMessages_ = chatController_.conversationMessages();
                finishGeneration(cancelled, metrics);
                updateState(workerClient_.state());
            });
    connect(&chatController_, &application::ChatController::errorOccurred, this,
            [this](const QString& code, const QString& message)
            {
                conversationMessages_ = chatController_.conversationMessages();
                showError(code, message);
                updateState(workerClient_.state());
            });
    connect(&agentController_,
            &application::AgentController::userRequestAccepted, this,
            [this](const QString&, const QString& prompt)
            {
                appendUserMessage(prompt);
                showAgentActivity();
                chatView_->promptEditor()->clear();
                pendingUtf8_.clear();
                currentAssistantText_.clear();
            });
    connect(&agentController_, &application::AgentController::finalAnswerReady,
            this, [this](const QString&, const QString& answer)
            { appendAgentAnswer(answer); });
    connect(&agentController_, &application::AgentController::approvalRequested,
            this,
            [this](const QString&, const QString& toolName,
                   const QJsonObject& arguments)
            {
                auto* approval = new ToolApprovalWidget(
                    toolPolicy_.risk(toolName), toolName, arguments);
                pendingToolApproval_ = approval;
                chatView_->conversationLayout()->insertWidget(
                    chatView_->conversationLayout()->count() - 1, approval);
                connect(
                    approval, &ToolApprovalWidget::decisionMade, this,
                    [this, approval, toolName](ToolApprovalDecision decision)
                    {
                        if (pendingToolApproval_ != approval) return;
                        pendingToolApproval_ = nullptr;
                        const auto approved =
                            decision != ToolApprovalDecision::DenyOnce;
                        if (decision == ToolApprovalDecision::AlwaysAllow)
                        {
                            auto rule = toolPolicy_.rule(toolName).value_or(
                                infrastructure::mcp::defaultToolPolicyRule(
                                    toolName));
                            rule.enabled = true;
                            rule.alwaysAllow = true;
                            toolPolicy_.setRule(toolName, rule);
                            auto alwaysAllowed =
                                settingsStore_.alwaysAllowedMcpTools();
                            if (!alwaysAllowed.contains(toolName))
                                alwaysAllowed.append(toolName);
                            if (!settingsStore_.setAlwaysAllowedMcpTools(
                                    alwaysAllowed))
                                qWarning().noquote()
                                    << "Unable to persist MCP permission:"
                                    << toolName;
                        }
                        agentController_.resolveApproval(approved);
                    });
                scrollConversationToBottom();
            });
    connect(&agentController_, &application::AgentController::eventRecorded,
            this,
            [this](const agent::Event& event)
            {
                appendAgentEvent(event);
                if (event.type == agent::EventType::Warning)
                {
                    stopThinkingAnimation();
                    updateAgentActivity(event.message);
                }
                if (!event.message.isEmpty())
                    chatView_->setStatusText(event.message);
            });
    connect(&agentController_, &application::AgentController::stateChanged,
            this, &MainWindow::updateAgentState);
    connect(&agentController_, &application::AgentController::runFinished, this,
            [this](const QString&, application::AgentRun::State state,
                   const QString& code, const QString& message)
            {
                agentRunActive_ = false;
                removeAgentActivity();
                if (pendingToolApproval_ != nullptr)
                {
                    pendingToolApproval_->markCancelled();
                    pendingToolApproval_ = nullptr;
                }
                if (state == application::AgentRun::State::Completed)
                {
                    conversationMessages_ =
                        agentController_.conversationMessages();
                    chatView_->setStatusText(tr("Ready"));
                }
                else if (!message.isEmpty())
                {
                    showError(code, message);
                }
                updateState(workerClient_.state());
            });
    connect(&mcpManager_,
            &infrastructure::mcp::McpClientManager::serverStateChanged, this,
            [this](const QString& serverId,
                   infrastructure::mcp::McpServerState state)
            {
                appendMcpDiagnostic(
                    serverId, tr("State"),
                    infrastructure::mcp::mcpServerStateName(state));
                refreshMcpServerMenu();
            });
    connect(&mcpManager_,
            &infrastructure::mcp::McpClientManager::capabilitySnapshotChanged,
            this, [this](const infrastructure::mcp::McpServerSnapshot&)
            { refreshMcpServerMenu(); });
    connect(&mcpManager_, &infrastructure::mcp::McpClientManager::serverStarted,
            this,
            [this](const QString& serverId)
            {
                mcpServerErrors_.remove(serverId);
                appendMcpDiagnostic(serverId, tr("Transport"), tr("Started"));
                refreshMcpServerMenu();
                mcpManager_.initialize(serverId);
            });
    connect(&mcpManager_, &infrastructure::mcp::McpClientManager::serverStopped,
            this,
            [this](const QString& serverId)
            {
                appendMcpDiagnostic(serverId, tr("Transport"), tr("Stopped"));
                if (pendingMcpRestarts_.remove(serverId))
                    QTimer::singleShot(0, this, [this, serverId]
                                       { startMcpServer(serverId); });
                refreshMcpServerMenu();
            });
    connect(&mcpManager_,
            &infrastructure::mcp::McpClientManager::serverInitialized, this,
            [this](const QString& serverId, const QJsonObject&)
            {
                appendMcpDiagnostic(serverId, tr("Protocol"),
                                    tr("Initialized"));
                refreshMcpServerMenu();
                const auto snapshot = mcpManager_.serverSnapshot(serverId);
                if (!snapshot) return;
                if (snapshot->capabilities.tools)
                    mcpManager_.listTools(serverId);
                if (snapshot->capabilities.resources)
                {
                    mcpManager_.listResources(serverId);
                    mcpManager_.listResourceTemplates(serverId);
                }
                if (snapshot->capabilities.prompts)
                    mcpManager_.listPrompts(serverId);
            });
    connect(&mcpManager_, &infrastructure::mcp::McpClientManager::toolsChanged,
            this,
            [this](const QString& serverId,
                   const QList<agent::ToolDefinition>& tools)
            {
                mcpServerErrors_.remove(serverId);
                appendMcpDiagnostic(
                    serverId, tr("Tools"),
                    tr("Catalog updated: %n tools", nullptr, tools.size()));
                const auto alwaysAllowed =
                    settingsStore_.alwaysAllowedMcpTools();
                for (const auto& tool : tools)
                {
                    auto rule = infrastructure::mcp::defaultToolPolicyRule(
                        tool.qualifiedName);
                    if (alwaysAllowed.contains(tool.qualifiedName))
                        rule.alwaysAllow = true;
                    toolPolicy_.setRule(tool.qualifiedName, rule);
                }
                refreshMcpServerMenu();
            });
    connect(
        &mcpManager_, &infrastructure::mcp::McpClientManager::resourcesChanged,
        this,
        [this](
            const QString& serverId,
            const QList<infrastructure::mcp::McpResourceDefinition>& resources)
        {
            appendMcpDiagnostic(
                serverId, tr("Resources"),
                tr("Catalog updated: %n resources", nullptr, resources.size()));
            refreshMcpServerMenu();
        });
    connect(
        &mcpManager_,
        &infrastructure::mcp::McpClientManager::resourceTemplatesChanged, this,
        [this](const QString& serverId,
               const QList<infrastructure::mcp::McpResourceTemplateDefinition>&
                   templates)
        {
            appendMcpDiagnostic(
                serverId, tr("Resources"),
                tr("Catalog updated: %n templates", nullptr, templates.size()));
            refreshMcpServerMenu();
        });
    connect(
        &mcpManager_, &infrastructure::mcp::McpClientManager::promptsChanged,
        this,
        [this](const QString& serverId,
               const QList<infrastructure::mcp::McpPromptDefinition>& prompts)
        {
            appendMcpDiagnostic(
                serverId, tr("Prompts"),
                tr("Catalog updated: %n prompts", nullptr, prompts.size()));
            refreshMcpServerMenu();
        });
    connect(&mcpManager_,
            &infrastructure::mcp::McpClientManager::resourceReadReady, this,
            [this](const infrastructure::mcp::McpResourceReadResult& result)
            {
                appendMcpDiagnostic(result.serverId, tr("Resources"),
                                    tr("Read %1").arg(result.requestedUri));
                if (mcpControlPanel_ != nullptr)
                    mcpControlPanel_->showResourceResult(result);
            });
    connect(&mcpManager_, &infrastructure::mcp::McpClientManager::promptReady,
            this,
            [this](const infrastructure::mcp::McpPromptResult& result)
            {
                appendMcpDiagnostic(result.serverId, tr("Prompts"),
                                    tr("Loaded %1").arg(result.promptName));
                if (mcpControlPanel_ != nullptr)
                    mcpControlPanel_->showPromptResult(result);
            });
    connect(&mcpManager_,
            &infrastructure::mcp::McpClientManager::resourceUpdated, this,
            [this](const QString& serverId, const QString& uri)
            {
                appendMcpDiagnostic(serverId, tr("Resources"),
                                    tr("Updated %1").arg(uri));
            });
    connect(&mcpManager_,
            &infrastructure::mcp::McpClientManager::resourceSubscriptionChanged,
            this,
            [this](const QString& serverId, const QString& uri, bool subscribed)
            {
                appendMcpDiagnostic(serverId, tr("Resources"),
                                    subscribed
                                        ? tr("Subscribed to %1").arg(uri)
                                        : tr("Unsubscribed from %1").arg(uri));
                refreshMcpServerMenu();
            });
    connect(&mcpManager_,
            &infrastructure::mcp::McpClientManager::rootsRequested, this,
            [this](const QString& serverId,
                   const QList<infrastructure::mcp::McpRoot>& roots)
            {
                appendMcpDiagnostic(
                    serverId, tr("Roots"),
                    tr("Returned %n authorized roots", nullptr, roots.size()));
            });
    connect(&mcpManager_, &infrastructure::mcp::McpClientManager::pingCompleted,
            this,
            [this](const QString& serverId, const QString&, qint64 elapsedMs)
            {
                appendMcpDiagnostic(serverId, tr("Ping"),
                                    tr("Completed in %1 ms").arg(elapsedMs));
            });
    connect(&mcpManager_, &infrastructure::mcp::McpClientManager::pingRequested,
            this,
            [this](const QString& serverId)
            {
                appendMcpDiagnostic(serverId, tr("Ping"),
                                    tr("Server request answered"));
            });
    connect(&mcpManager_, &infrastructure::mcp::McpClientManager::serverError,
            this,
            [this](const QString& serverId, const QString& code,
                   const QString& message)
            {
                const auto safeMessage = redactSensitiveText(message);
                mcpServerErrors_.insert(serverId, safeMessage);
                appendMcpDiagnostic(serverId, code, safeMessage);
                refreshMcpServerMenu();
                qWarning().noquote() << serverId << code << safeMessage;
            });
    connect(
        &mcpManager_, &infrastructure::mcp::McpClientManager::requestFailed,
        this,
        [this](const QString& serverId, const QString&, const QString& method,
               const QString& code, const QString& message)
        {
            if (method == QLatin1String("tools/call")) return;
            const auto safeMessage = redactSensitiveText(message);
            mcpServerErrors_.insert(serverId, safeMessage);
            appendMcpDiagnostic(serverId,
                                QStringLiteral("%1/%2").arg(method, code),
                                safeMessage);
            refreshMcpServerMenu();
            qWarning().noquote() << serverId << method << code << safeMessage;
        });
    connect(&mcpManager_,
            &infrastructure::mcp::McpClientManager::diagnosticReceived, this,
            [this](const QString& serverId, const QString& text)
            { appendMcpDiagnostic(serverId, tr("stderr"), text); });
    connect(
        &mcpManager_,
        &infrastructure::mcp::McpClientManager::loggingMessageReceived, this,
        [this](const QString& serverId, const QString& level,
               const QString& logger, const QJsonValue& data)
        {
            const auto safe = redactSensitiveValues(data);
            const auto serialized = QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("data"), safe}})
                    .toJson(QJsonDocument::Compact));
            appendMcpDiagnostic(
                serverId,
                logger.isEmpty() ? level
                                 : QStringLiteral("%1/%2").arg(level, logger),
                serialized);
        });
    connect(
        &mcpManager_, &infrastructure::mcp::McpClientManager::toolResultReady,
        &agentController_, &application::AgentController::receiveToolResult);
    loadMcpServers();
    workerClient_.start();
}

void MainWindow::selectModelPackage()
{
    const QFileInfo current(modelPath_);
    auto initialDirectory = QDir::homePath();
    if (current.isDir())
        initialDirectory = current.absoluteFilePath();
    else if (current.isFile())
        initialDirectory = current.absolutePath();
    const auto selected = QFileDialog::getExistingDirectory(
        this, tr("Select model folder"), initialDirectory,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) return;

    const auto selectedPath = QFileInfo(selected).canonicalFilePath();
    const auto reloadImmediately =
        !activeModelSelection_.selectedPath.isEmpty();
    setModelPath(selectedPath);
    if (reloadImmediately) loadSelectedModel();
}

void MainWindow::openModelDirectory()
{
    if (modelPath_.isEmpty()) return;

    const QFileInfo model(modelPath_);
    const auto directory =
        model.isDir() ? model.absoluteFilePath() : model.absolutePath();
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
    {
        chatView_->setStatusText(tr("Unable to open the model folder"));
        qWarning().noquote() << "Unable to open model folder:" << directory;
    }
}

void MainWindow::selectWorkspaceDirectory()
{
    if (agentRunActive_ || filesystemConfiguredExternally_) return;
    const auto selected = QFileDialog::getExistingDirectory(
        this, tr("Select workspace folder"), workspacePath_,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) return;

    const auto workspacePath = QFileInfo(selected).canonicalFilePath();
    if (workspacePath.isEmpty())
    {
        showError(QStringLiteral("invalid_workspace"),
                  tr("The selected workspace folder is not available."));
        return;
    }
    if (workspacePath == workspacePath_) return;

    QString errorMessage;
    if (settingsStore_.builtInFilesystemMcpEnabled() &&
        !startBuiltInFilesystem(workspacePath, errorMessage))
    {
        showError(QStringLiteral("filesystem_mcp_unavailable"), errorMessage);
        return;
    }
    workspacePath_ = workspacePath;
    chatView_->setWorkspacePresentation(workspacePath_);
    if (!settingsStore_.setWorkspacePath(workspacePath))
        qWarning().noquote()
            << "Unable to persist workspace path:" << workspacePath;
    refreshMcpServerMenu();
}

void MainWindow::openWorkspaceDirectory()
{
    if (workspacePath_.isEmpty()) return;
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(workspacePath_)))
    {
        chatView_->setStatusText(tr("Unable to open the workspace folder"));
        qWarning().noquote()
            << "Unable to open workspace folder:" << workspacePath_;
    }
}

void MainWindow::addMcpServer()
{
    if (agentRunActive_ || chatController_.isGenerating()) return;

    auto existingIds = QStringList{QStringLiteral("filesystem")};
    for (const auto& config : mcpConfigurations_)
        existingIds.append(config.serverId);
    existingIds.removeDuplicates();

    McpServerDialog dialog(existingIds, this);
    if (dialog.exec() != QDialog::Accepted) return;

    auto config = dialog.configuration();
    QString errorMessage;
    if (!mcpManager_.addServer(config, errorMessage))
    {
        showError(QStringLiteral("mcp_config_rejected"), errorMessage);
        return;
    }

    mcpConfigurations_.append(config);
    if (!settingsStore_.setMcpServerConfigs(mcpConfigurations_))
    {
        mcpConfigurations_.removeLast();
        QString removalError;
        mcpManager_.removeServer(config.serverId, removalError);
        showError(QStringLiteral("mcp_config_not_saved"),
                  tr("The MCP server configuration could not be saved."));
        return;
    }

    mcpServerErrors_.remove(config.serverId);
    mcpManager_.startServer(config.serverId);
    refreshMcpServerMenu();
}

void MainWindow::setBuiltInFilesystemMcpEnabled(bool enabled)
{
    if (agentRunActive_ || chatController_.isGenerating())
    {
        refreshMcpServerMenu();
        return;
    }
    if (filesystemConfiguredExternally_)
    {
        refreshMcpServerMenu();
        return;
    }

    if (enabled)
    {
        QString errorMessage;
        if (!startBuiltInFilesystem(workspacePath_, errorMessage))
        {
            showError(QStringLiteral("filesystem_mcp_unavailable"),
                      errorMessage);
            refreshMcpServerMenu();
            return;
        }
        if (!settingsStore_.setBuiltInFilesystemMcpEnabled(true))
        {
            QString removalError;
            mcpManager_.removeServer(QStringLiteral("filesystem"),
                                     removalError);
            builtInFilesystemRunning_ = false;
            showError(QStringLiteral("mcp_config_not_saved"),
                      tr("The built-in MCP selection could not be saved."));
        }
    }
    else
    {
        if (!settingsStore_.setBuiltInFilesystemMcpEnabled(false))
        {
            showError(QStringLiteral("mcp_config_not_saved"),
                      tr("The built-in MCP selection could not be saved."));
            refreshMcpServerMenu();
            return;
        }
        if (builtInFilesystemRunning_)
        {
            QString errorMessage;
            if (!mcpManager_.removeServer(QStringLiteral("filesystem"),
                                          errorMessage))
            {
                settingsStore_.setBuiltInFilesystemMcpEnabled(true);
                showError(QStringLiteral("mcp_stop_failed"), errorMessage);
                refreshMcpServerMenu();
                return;
            }
            builtInFilesystemRunning_ = false;
        }
        mcpServerErrors_.remove(QStringLiteral("filesystem"));
    }
    refreshMcpServerMenu();
}

void MainWindow::setExternalMcpServerEnabled(const QString& serverId,
                                             bool enabled)
{
    if (agentRunActive_ || chatController_.isGenerating())
    {
        refreshMcpServerMenu();
        return;
    }

    auto iterator = std::find_if(
        mcpConfigurations_.begin(), mcpConfigurations_.end(),
        [&serverId](const infrastructure::mcp::McpServerConfig& config)
        { return config.serverId == serverId; });
    if (iterator == mcpConfigurations_.end() || iterator->enabled == enabled)
    {
        refreshMcpServerMenu();
        return;
    }

    const auto previous = iterator->enabled;
    iterator->enabled = enabled;
    if (!settingsStore_.setMcpServerConfigs(mcpConfigurations_))
    {
        iterator->enabled = previous;
        showError(QStringLiteral("mcp_config_not_saved"),
                  tr("The MCP server selection could not be saved."));
        refreshMcpServerMenu();
        return;
    }

    QString errorMessage;
    if (enabled)
    {
        if (!mcpManager_.addServer(*iterator, errorMessage))
        {
            iterator->enabled = false;
            settingsStore_.setMcpServerConfigs(mcpConfigurations_);
            showError(QStringLiteral("mcp_start_failed"), errorMessage);
            refreshMcpServerMenu();
            return;
        }
        mcpServerErrors_.remove(serverId);
        mcpManager_.startServer(serverId);
    }
    else
    {
        if (mcpManager_.serverIds().contains(serverId) &&
            !mcpManager_.removeServer(serverId, errorMessage))
        {
            iterator->enabled = true;
            settingsStore_.setMcpServerConfigs(mcpConfigurations_);
            showError(QStringLiteral("mcp_stop_failed"), errorMessage);
            refreshMcpServerMenu();
            return;
        }
        mcpServerErrors_.remove(serverId);
    }
    refreshMcpServerMenu();
}

void MainWindow::removeExternalMcpServer(const QString& serverId)
{
    if (agentRunActive_ || chatController_.isGenerating()) return;

    const auto iterator = std::find_if(
        mcpConfigurations_.cbegin(), mcpConfigurations_.cend(),
        [&serverId](const infrastructure::mcp::McpServerConfig& config)
        { return config.serverId == serverId; });
    if (iterator == mcpConfigurations_.cend())
    {
        refreshMcpServerMenu();
        return;
    }

    const auto confirmation = QMessageBox::question(
        this, tr("Remove MCP server"),
        tr("Remove MCP server '%1'? The server files will not be deleted.")
            .arg(serverId),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (confirmation != QMessageBox::Yes) return;

    auto updatedConfigurations = mcpConfigurations_;
    updatedConfigurations.removeAt(
        std::distance(mcpConfigurations_.cbegin(), iterator));
    if (!settingsStore_.setMcpServerConfigs(updatedConfigurations))
    {
        showError(QStringLiteral("mcp_config_not_saved"),
                  tr("The MCP server configuration could not be removed."));
        return;
    }

    QString errorMessage;
    if (mcpManager_.serverIds().contains(serverId) &&
        !mcpManager_.removeServer(serverId, errorMessage))
    {
        settingsStore_.setMcpServerConfigs(mcpConfigurations_);
        showError(QStringLiteral("mcp_stop_failed"), errorMessage);
        return;
    }

    mcpConfigurations_ = std::move(updatedConfigurations);
    mcpServerErrors_.remove(serverId);
    mcpDiagnostics_.remove(serverId);
    filesystemConfiguredExternally_ = std::any_of(
        mcpConfigurations_.cbegin(), mcpConfigurations_.cend(),
        [](const infrastructure::mcp::McpServerConfig& config)
        { return config.serverId.trimmed() == QLatin1String("filesystem"); });
    if (!filesystemConfiguredExternally_ &&
        settingsStore_.builtInFilesystemMcpEnabled() &&
        !builtInFilesystemRunning_)
    {
        if (!startBuiltInFilesystem(workspacePath_, errorMessage))
        {
            const auto safeMessage = redactSensitiveText(errorMessage);
            mcpServerErrors_.insert(QStringLiteral("filesystem"), safeMessage);
            showError(QStringLiteral("filesystem_mcp_unavailable"),
                      safeMessage);
        }
    }
    updateState(workerClient_.state());
    refreshMcpServerMenu();
}

void MainWindow::showMcpControlPanel()
{
    if (mcpControlPanel_ != nullptr)
    {
        mcpControlPanel_->show();
        mcpControlPanel_->raise();
        mcpControlPanel_->activateWindow();
        return;
    }

    mcpControlPanel_ = new McpControlPanel(this);
    mcpControlPanel_->setAttribute(Qt::WA_DeleteOnClose);
    connect(mcpControlPanel_, &QObject::destroyed, this,
            [this] { mcpControlPanel_ = nullptr; });
    connect(mcpControlPanel_, &McpControlPanel::addServerRequested, this,
            &MainWindow::addMcpServer);
    connect(mcpControlPanel_, &McpControlPanel::removeServerRequested, this,
            &MainWindow::removeExternalMcpServer);
    connect(mcpControlPanel_, &McpControlPanel::serverEnabledChanged, this,
            [this](const QString& serverId, bool builtIn, bool enabled)
            {
                if (builtIn)
                    setBuiltInFilesystemMcpEnabled(enabled);
                else
                    setExternalMcpServerEnabled(serverId, enabled);
            });
    connect(mcpControlPanel_, &McpControlPanel::startServerRequested, this,
            &MainWindow::startMcpServer);
    connect(mcpControlPanel_, &McpControlPanel::stopServerRequested, this,
            &MainWindow::stopMcpServer);
    connect(mcpControlPanel_, &McpControlPanel::restartServerRequested, this,
            &MainWindow::restartMcpServer);
    connect(mcpControlPanel_, &McpControlPanel::pingServerRequested, this,
            &MainWindow::pingMcpServer);
    connect(mcpControlPanel_, &McpControlPanel::refreshToolsRequested, this,
            &MainWindow::refreshMcpTools);
    connect(mcpControlPanel_, &McpControlPanel::readResourceRequested, this,
            &MainWindow::readMcpResource);
    connect(mcpControlPanel_, &McpControlPanel::resourceSubscriptionRequested,
            this, &MainWindow::setMcpResourceSubscribed);
    connect(mcpControlPanel_, &McpControlPanel::getPromptRequested, this,
            &MainWindow::getMcpPrompt);
    refreshMcpControlPanel();
    mcpControlPanel_->show();
}

void MainWindow::startMcpServer(const QString& serverId)
{
    if (agentRunActive_ || chatController_.isGenerating()) return;
    const auto snapshot = mcpManager_.serverSnapshot(serverId);
    if (!snapshot ||
        (snapshot->state != infrastructure::mcp::McpServerState::Stopped &&
         snapshot->state != infrastructure::mcp::McpServerState::Failed))
        return;
    mcpServerErrors_.remove(serverId);
    mcpManager_.startServer(serverId);
    refreshMcpServerMenu();
}

void MainWindow::stopMcpServer(const QString& serverId)
{
    if (agentRunActive_ || chatController_.isGenerating()) return;
    pendingMcpRestarts_.remove(serverId);
    if (mcpManager_.serverIds().contains(serverId))
        mcpManager_.stopServer(serverId);
    refreshMcpServerMenu();
}

void MainWindow::restartMcpServer(const QString& serverId)
{
    if (agentRunActive_ || chatController_.isGenerating()) return;
    const auto snapshot = mcpManager_.serverSnapshot(serverId);
    if (!snapshot) return;
    if (snapshot->state == infrastructure::mcp::McpServerState::Stopped ||
        snapshot->state == infrastructure::mcp::McpServerState::Failed)
    {
        startMcpServer(serverId);
        return;
    }
    if (snapshot->state == infrastructure::mcp::McpServerState::Starting ||
        snapshot->state == infrastructure::mcp::McpServerState::Initializing ||
        snapshot->state == infrastructure::mcp::McpServerState::Stopping)
        return;
    pendingMcpRestarts_.insert(serverId);
    mcpManager_.stopServer(serverId);
    refreshMcpServerMenu();
}

void MainWindow::pingMcpServer(const QString& serverId)
{
    if (agentRunActive_ || chatController_.isGenerating()) return;
    const auto requestId = mcpManager_.ping(serverId);
    if (!requestId.isEmpty())
        appendMcpDiagnostic(serverId, tr("Ping"), tr("Request sent"));
}

void MainWindow::refreshMcpTools(const QString& serverId)
{
    if (agentRunActive_ || chatController_.isGenerating()) return;
    const auto snapshot = mcpManager_.serverSnapshot(serverId);
    if (!snapshot ||
        (snapshot->state != infrastructure::mcp::McpServerState::Ready &&
         snapshot->state != infrastructure::mcp::McpServerState::Degraded))
        return;
    if (snapshot->capabilities.tools) mcpManager_.listTools(serverId);
    if (snapshot->capabilities.resources)
    {
        mcpManager_.listResources(serverId);
        mcpManager_.listResourceTemplates(serverId);
    }
    if (snapshot->capabilities.prompts) mcpManager_.listPrompts(serverId);
}

void MainWindow::readMcpResource(const QString& serverId, const QString& uri)
{
    if (agentRunActive_ || chatController_.isGenerating()) return;
    mcpManager_.readResource(serverId, uri);
}

void MainWindow::setMcpResourceSubscribed(const QString& serverId,
                                          const QString& uri, bool subscribe)
{
    if (agentRunActive_ || chatController_.isGenerating()) return;
    if (subscribe)
        mcpManager_.subscribeResource(serverId, uri);
    else
        mcpManager_.unsubscribeResource(serverId, uri);
}

void MainWindow::getMcpPrompt(const QString& serverId, const QString& name,
                              const QJsonObject& arguments)
{
    if (agentRunActive_ || chatController_.isGenerating()) return;
    mcpManager_.getPrompt(serverId, name, arguments);
}

void MainWindow::loadSelectedModel()
{
    if (verifyingModelPackage_) return;

    auto result = models::ModelPackage::inspect(
        modelPath_, QCoreApplication::applicationVersion());
    if (!result.succeeded())
    {
        modelInfoText_ = tr("Invalid model selection");
        chatView_->setModelPresentation(modelPath_, modelInfoText_,
                                        ModelBadgeState::Invalid);
        showError(result.errorCode, result.errorMessage);
        return;
    }

    if (result.selection.descriptor.origin == models::ModelOrigin::DirectGguf)
    {
        beginModelLoad(std::move(result.selection));
        return;
    }

    if (modelHashesAreCached(result.selection))
    {
        result.selection.descriptor.verificationStatus =
            models::VerificationStatus::Verified;
        beginModelLoad(std::move(result.selection));
        return;
    }

    pendingModelSelection_ = result.selection;
    verifyingModelPackage_ = true;
    modelVerificationPercent_ = -1;
    modelInfoText_ = tr("Verifying package - %1")
                         .arg(result.selection.descriptor.displayName);
    chatView_->setModelPresentation(modelPath_, modelInfoText_,
                                    ModelBadgeState::Checking);
    updateState(workerClient_.state());
    const QPointer<MainWindow> window(this);
    modelVerificationWatcher_.setFuture(QtConcurrent::run(
        [selection = std::move(result.selection), window]() mutable
        {
            return models::ModelPackage::verify(
                std::move(selection),
                [window](quint64 verifiedBytes, quint64 totalBytes)
                {
                    if (!window) return;
                    QMetaObject::invokeMethod(
                        window,
                        [window, verifiedBytes, totalBytes]
                        {
                            if (window)
                                window->updateModelVerificationProgress(
                                    verifiedBytes, totalBytes);
                        },
                        Qt::QueuedConnection);
                });
        }));
}

void MainWindow::finishModelPackageVerification()
{
    auto result = modelVerificationWatcher_.result();
    verifyingModelPackage_ = false;
    modelVerificationPercent_ = -1;
    if (!result.succeeded())
    {
        pendingModelSelection_ = {};
        modelInfoText_ = tr("Invalid model package");
        chatView_->setModelPresentation(modelPath_, modelInfoText_,
                                        ModelBadgeState::Invalid);
        updateState(workerClient_.state());
        showError(result.errorCode, result.errorMessage);
        return;
    }

    if (!cacheVerifiedModel(result.selection))
    {
        qWarning() << "Unable to persist the model verification cache.";
    }
    beginModelLoad(std::move(result.selection));
}

void MainWindow::updateModelVerificationProgress(quint64 verifiedBytes,
                                                 quint64 totalBytes)
{
    if (!verifyingModelPackage_ || totalBytes == 0) return;
    const auto percent =
        qBound(0,
               static_cast<int>(100.0 * static_cast<double>(verifiedBytes) /
                                static_cast<double>(totalBytes)),
               100);
    if (percent == modelVerificationPercent_) return;
    modelVerificationPercent_ = percent;
    chatView_->setStatusText(tr("Verifying model package... %1%").arg(percent));
}

void MainWindow::triggerPrimaryAction()
{
    if (primaryActionStops_)
        stopGeneration();
    else
        sendPrompt();
}

void MainWindow::sendPrompt()
{
    const auto prompt = chatView_->promptEditor()->toPlainText().trimmed();
    if (prompt.isEmpty()) return;
    if (chatView_->isAgentModeSelected())
        beginAgentPrompt(prompt);
    else
        beginChatPrompt(prompt);
}

void MainWindow::stopGeneration()
{
    if (agentRunActive_)
        agentController_.cancel();
    else if (chatController_.isGenerating())
        chatController_.cancel();
}

void MainWindow::clearConversation()
{
    if (agentRunActive_ || chatController_.isGenerating()) return;
    if (!agentController_.clearConversation() ||
        !chatController_.clearConversation())
        return;
    conversationMessages_.clear();
    resetConversationView();
}

void MainWindow::resetConversationView()
{
    auto* conversationLayout = chatView_->conversationLayout();
    while (conversationLayout->count() > 1)
    {
        auto* item = conversationLayout->takeAt(0);
        delete item->widget();
        delete item;
    }

    chatView_->activityLog()->clear();
    renderTimer_->stop();
    currentAssistant_ = nullptr;
    agentActivityMessage_ = nullptr;
    pendingToolApproval_ = nullptr;
    currentAssistantText_.clear();
    pendingUtf8_.clear();
    chatView_->setStatusText(tr("Conversation cleared"));
    updateClearButton();
}

void MainWindow::updateState(infrastructure::WorkerClient::State state)
{
    const auto workerReady =
        state == infrastructure::WorkerClient::State::Ready ||
        state == infrastructure::WorkerClient::State::ModelReady;
    const auto ready = workerReady && !verifyingModelPackage_;
    const auto modelReady =
        state == infrastructure::WorkerClient::State::ModelReady &&
        !verifyingModelPackage_;
    const auto generating =
        state == infrastructure::WorkerClient::State::Generating;
    const auto conversationBusy =
        agentRunActive_ || chatController_.isGenerating();
    chatView_->setModelControlsEnabled(ready, ready && !modelPath_.isEmpty());
    chatView_->setWorkspaceControlsEnabled(!conversationBusy &&
                                           !filesystemConfiguredExternally_);
    chatView_->setPromptEnabled(modelReady && !conversationBusy);
    chatView_->setModeSelectionEnabled(modelReady && !conversationBusy);
    chatView_->setMcpSelectionEnabled(!conversationBusy);
    const auto stopMode = generating || conversationBusy;
    updatePrimaryAction(stopMode);
    chatView_->setPrimaryAction(stopMode, stopMode || modelReady);
    chatView_->setConversationVisible(modelReady || generating ||
                                      conversationBusy);
    updateClearButton();
    refreshMcpControlPanel();

    if (verifyingModelPackage_)
    {
        chatView_->setStatusText(tr("Verifying model package..."));
        return;
    }

    switch (state)
    {
        case infrastructure::WorkerClient::State::Stopped:
            chatView_->setStatusText(tr("Worker stopped"));
            break;
        case infrastructure::WorkerClient::State::Starting:
            chatView_->setStatusText(tr("Starting worker..."));
            break;
        case infrastructure::WorkerClient::State::Ready:
            chatView_->setStatusText({});
            break;
        case infrastructure::WorkerClient::State::UnloadingModel:
            chatView_->setStatusText(tr("Releasing current model..."));
            break;
        case infrastructure::WorkerClient::State::LoadingModel:
            chatView_->setStatusText(replacingModel_
                                         ? tr("Loading new model...")
                                         : tr("Loading model..."));
            break;
        case infrastructure::WorkerClient::State::ModelReady:
            chatView_->setStatusText(tr("Model ready"));
            break;
        case infrastructure::WorkerClient::State::Generating:
            chatView_->setStatusText(tr("Generating..."));
            break;
        case infrastructure::WorkerClient::State::Failed:
            chatView_->setStatusText(tr("Worker unavailable"));
            break;
    }
}

void MainWindow::updatePrimaryAction(bool stopMode)
{
    primaryActionStops_ = stopMode;
}

void MainWindow::appendToken(const QByteArray& bytes)
{
    pendingUtf8_ += bytes;
    flushPendingUtf8();
}

void MainWindow::finishGeneration(bool cancelled, const QJsonObject& metrics)
{
    flushPendingUtf8(true);
    renderTimer_->stop();
    renderAssistant(true);
    currentAssistant_ = nullptr;

    if (cancelled)
    {
        chatView_->setStatusText(tr("Generation stopped"));
        updateClearButton();
        return;
    }

    const auto discardedMessages =
        metrics.value(QStringLiteral("discardedMessages")).toInt();
    chatView_->setStatusText(
        discardedMessages > 0
            ? tr("Ready - %1 token/s - %2 earlier messages omitted")
                  .arg(metrics.value(QStringLiteral("tokensPerSecond"))
                           .toDouble(),
                       0, 'f', 1)
                  .arg(discardedMessages)
            : tr("Ready - %1 token/s")
                  .arg(metrics.value(QStringLiteral("tokensPerSecond"))
                           .toDouble(),
                       0, 'f', 1));
    updateClearButton();
}

void MainWindow::showError(const QString& code, const QString& message)
{
    removeAgentActivity();
    chatView_->setStatusText(tr("Error: %1").arg(message));
    if (currentAssistant_ != nullptr)
    {
        currentAssistantText_ +=
            QStringLiteral("\n\n**%1:** %2").arg(tr("Error"), message);
        renderTimer_->stop();
        renderAssistant(true);
        currentAssistant_ = nullptr;
    }
    updateClearButton();
    qWarning().noquote() << code << message;
}

void MainWindow::buildUi()
{
    chatView_ = new ChatView(this);
    chatView_->setObjectName(QStringLiteral("centralView"));
    setCentralWidget(chatView_);

    renderTimer_ = new QTimer(this);
    renderTimer_->setSingleShot(true);
    renderTimer_->setInterval(40);
    connect(renderTimer_, &QTimer::timeout, this,
            [this] { renderAssistant(); });

    thinkingAnimationTimer_ = new QTimer(this);
    thinkingAnimationTimer_->setObjectName(
        QStringLiteral("thinkingAnimationTimer"));
    thinkingAnimationTimer_->setInterval(360);
    connect(thinkingAnimationTimer_, &QTimer::timeout, this,
            &MainWindow::advanceThinkingAnimation);

    connect(chatView_, &ChatView::modelFolderRequested, this,
            &MainWindow::selectModelPackage);
    connect(chatView_, &ChatView::modelLocationRequested, this,
            &MainWindow::openModelDirectory);
    connect(chatView_, &ChatView::workspaceFolderRequested, this,
            &MainWindow::selectWorkspaceDirectory);
    connect(chatView_, &ChatView::workspaceOpenRequested, this,
            &MainWindow::openWorkspaceDirectory);
    connect(chatView_, &ChatView::addMcpServerRequested, this,
            &MainWindow::addMcpServer);
    connect(chatView_, &ChatView::manageMcpServersRequested, this,
            &MainWindow::showMcpControlPanel);
    connect(chatView_, &ChatView::builtInFilesystemMcpToggled, this,
            &MainWindow::setBuiltInFilesystemMcpEnabled);
    connect(chatView_, &ChatView::externalMcpServerToggled, this,
            &MainWindow::setExternalMcpServerEnabled);
    connect(chatView_, &ChatView::removeExternalMcpServerRequested, this,
            &MainWindow::removeExternalMcpServer);
    connect(chatView_, &ChatView::modelLoadRequested, this,
            &MainWindow::loadSelectedModel);
    connect(chatView_, &ChatView::primaryActionRequested, this,
            &MainWindow::triggerPrimaryAction);
    connect(chatView_, &ChatView::promptSubmitted, this,
            &MainWindow::sendPrompt);
    connect(chatView_, &ChatView::clearConversationRequested, this,
            &MainWindow::clearConversation);
    connect(chatView_, &ChatView::modeChanged, this,
            [this](bool agentMode)
            {
                if (!settingsStore_.setAgentModeEnabled(agentMode))
                    qWarning() << "Unable to persist conversation mode.";
            });
    updatePrimaryAction(false);
    updateState(infrastructure::WorkerClient::State::Stopped);
}

void MainWindow::appendUserMessage(const QString& text)
{
    auto* message = new MessageWidget(MessageWidget::Role::User);
    message->setUserText(text);
    chatView_->conversationLayout()->insertWidget(
        chatView_->conversationLayout()->count() - 1, message);

    updateClearButton();
    scrollConversationToBottom();
}

void MainWindow::beginAssistantMessage()
{
    currentAssistant_ = new MessageWidget(MessageWidget::Role::Assistant);
    currentAssistant_->setAssistantText({}, false);
    chatView_->conversationLayout()->insertWidget(
        chatView_->conversationLayout()->count() - 1, currentAssistant_);
    scrollConversationToBottom();
}

void MainWindow::appendActivityText(const QString& text)
{
    auto cursor = chatView_->activityLog()->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    chatView_->activityLog()->setTextCursor(cursor);
    chatView_->activityLog()->ensureCursorVisible();
}

void MainWindow::appendAgentEvent(const agent::Event& event)
{
    QStringList lines;
    if (event.type == agent::EventType::RunStarted)
    {
        if (!chatView_->activityLog()->document()->isEmpty())
            lines.append(QString{});
        lines.append(QStringLiteral("===================="));
    }

    const auto timestamp =
        event.timestamp.toLocalTime().toString(QStringLiteral("HH:mm:ss"));
    const auto message = event.message.isEmpty()
                             ? agent::eventTypeName(event.type)
                             : event.message;
    lines.append(QStringLiteral("[%1] %2").arg(timestamp, message));
    if (!event.toolName.isEmpty())
        lines.append(tr("Tool: %1").arg(event.toolName));
    if (!event.data.isEmpty())
    {
        lines.append(tr("Data:"));
        lines.append(
            QString::fromUtf8(
                QJsonDocument(redactSensitiveValues(event.data).toObject())
                    .toJson(QJsonDocument::Indented))
                .trimmed());
    }
    appendActivityText(lines.join(QLatin1Char('\n')) + QStringLiteral("\n\n"));
}

void MainWindow::renderAssistant(bool final)
{
    if (currentAssistant_ == nullptr) return;
    const auto followOutput = conversationIsAtBottom();
    currentAssistant_->setAssistantText(currentAssistantText_, final);
    if (followOutput) scrollConversationToBottom();
}

bool MainWindow::conversationIsAtBottom() const
{
    const auto* scrollBar =
        chatView_->conversationScroll()->verticalScrollBar();
    return scrollBar->maximum() - scrollBar->value() <= 48;
}

void MainWindow::scrollConversationToBottom()
{
    QTimer::singleShot(
        0, chatView_->conversationScroll(),
        [this]
        {
            auto* scrollBar =
                chatView_->conversationScroll()->verticalScrollBar();
            scrollBar->setValue(scrollBar->maximum());
        });
}

void MainWindow::flushPendingUtf8(bool final)
{
    if (pendingUtf8_.isEmpty()) return;

    auto validLength = pendingUtf8_.size();
    if (!final)
    {
        auto continuationBytes = 0;
        while (continuationBytes < qMin(3, validLength) &&
               (static_cast<unsigned char>(
                    pendingUtf8_.at(validLength - continuationBytes - 1)) &
                0xC0U) == 0x80U)
            ++continuationBytes;
        const auto leadingIndex = validLength - continuationBytes - 1;
        if (leadingIndex >= 0)
        {
            const auto leading =
                static_cast<unsigned char>(pendingUtf8_.at(leadingIndex));
            auto sequenceLength = 1;
            if ((leading & 0xE0U) == 0xC0U)
                sequenceLength = 2;
            else if ((leading & 0xF0U) == 0xE0U)
                sequenceLength = 3;
            else if ((leading & 0xF8U) == 0xF0U)
                sequenceLength = 4;
            if (continuationBytes + 1 < sequenceLength)
                validLength = leadingIndex;
        }
    }
    if (validLength <= 0) return;

    const auto text = QString::fromUtf8(pendingUtf8_.constData(), validLength);
    pendingUtf8_.remove(0, validLength);
    currentAssistantText_ += text;
    if (!final && !renderTimer_->isActive()) renderTimer_->start();
}

void MainWindow::updateClearButton()
{
    const auto hasVisibleMessages =
        chatView_->conversationLayout()->count() > 1;
    chatView_->setClearEnabled(
        !agentRunActive_ && !chatController_.isGenerating() &&
        (!conversationMessages_.isEmpty() ||
         agentController_.hasConversation() ||
         chatController_.hasConversation() || hasVisibleMessages));
}

void MainWindow::beginChatPrompt(const QString& prompt)
{
    if (agentRunActive_ || chatController_.isGenerating()) return;
    if (!chatController_.setConversationMessages(conversationMessages_)) return;

    const application::AssistantContext context{
        activeModelSelection_.descriptor.displayName, workspacePath_};
    if (!chatController_.sendPrompt(prompt, activeModelSelection_.preset,
                                    context))
    {
        showError(QStringLiteral("chat_unavailable"), tr("Chat is not ready."));
    }
}

void MainWindow::beginAgentPrompt(const QString& prompt)
{
    if (agentRunActive_ || chatController_.isGenerating()) return;
    if (mcpManager_.tools().isEmpty())
    {
        showError(QStringLiteral("agent_tools_unavailable"),
                  tr("Agent tools are not available."));
        return;
    }
    if (!agentController_.setConversationMessages(conversationMessages_))
        return;
    agentRunActive_ = true;
    updateState(workerClient_.state());
    const application::AssistantContext context{
        activeModelSelection_.descriptor.displayName, workspacePath_,
        mcpManager_.agentInstructions()};
    if (!agentController_.start(prompt, activeModelSelection_.preset,
                                mcpManager_.tools(), context))
    {
        agentRunActive_ = false;
        showError(QStringLiteral("agent_unavailable"),
                  tr("Agent dependencies are not ready."));
        updateState(workerClient_.state());
    }
}

void MainWindow::appendAgentAnswer(const QString& answer)
{
    removeAgentActivity();
    auto* message = new MessageWidget(MessageWidget::Role::Assistant);
    message->setAssistantText(answer, true);
    chatView_->conversationLayout()->insertWidget(
        chatView_->conversationLayout()->count() - 1, message);
    updateClearButton();
    scrollConversationToBottom();
}

void MainWindow::showAgentActivity()
{
    if (agentActivityMessage_ != nullptr) return;
    agentActivityMessage_ = new MessageWidget(MessageWidget::Role::Assistant);
    agentActivityMessage_->setProperty("agentActivity", true);
    chatView_->conversationLayout()->insertWidget(
        chatView_->conversationLayout()->count() - 1, agentActivityMessage_);
    startThinkingAnimation();
    scrollConversationToBottom();
}

void MainWindow::startThinkingAnimation()
{
    if (agentActivityMessage_ == nullptr) return;
    if (thinkingAnimationTimer_->isActive()) return;
    thinkingAnimationFrame_ = 1;
    advanceThinkingAnimation();
    thinkingAnimationTimer_->start();
}

void MainWindow::stopThinkingAnimation()
{
    if (thinkingAnimationTimer_->isActive()) thinkingAnimationTimer_->stop();
    thinkingAnimationFrame_ = 1;
}

void MainWindow::advanceThinkingAnimation()
{
    if (agentActivityMessage_ == nullptr)
    {
        stopThinkingAnimation();
        return;
    }
    updateAgentActivity(
        tr("Thinking%1")
            .arg(QString(thinkingAnimationFrame_, QLatin1Char('.'))));
    thinkingAnimationFrame_ = thinkingAnimationFrame_ % 3 + 1;
}

void MainWindow::updateAgentActivity(const QString& text)
{
    if (agentActivityMessage_ == nullptr) return;
    const auto followOutput = conversationIsAtBottom();
    agentActivityMessage_->setAssistantText(text, false);
    if (followOutput) scrollConversationToBottom();
}

void MainWindow::removeAgentActivity()
{
    stopThinkingAnimation();
    if (agentActivityMessage_ == nullptr) return;
    chatView_->conversationLayout()->removeWidget(agentActivityMessage_);
    delete agentActivityMessage_;
    agentActivityMessage_ = nullptr;
}

void MainWindow::updateAgentState(application::AgentRun::State state)
{
    switch (state)
    {
        case application::AgentRun::State::Deciding:
            chatView_->setStatusText(tr("Thinking..."));
            startThinkingAnimation();
            break;
        case application::AgentRun::State::WaitingForApproval:
            stopThinkingAnimation();
            chatView_->setStatusText(tr("Waiting for tool approval"));
            updateAgentActivity(tr("Waiting for tool approval..."));
            break;
        case application::AgentRun::State::ExecutingTool:
            stopThinkingAnimation();
            chatView_->setStatusText(tr("Using a tool..."));
            updateAgentActivity(tr("Using a tool..."));
            break;
        case application::AgentRun::State::GeneratingAnswer:
            stopThinkingAnimation();
            chatView_->setStatusText(tr("Preparing the answer..."));
            updateAgentActivity(tr("Preparing the answer..."));
            break;
        case application::AgentRun::State::Completed:
        case application::AgentRun::State::Cancelled:
        case application::AgentRun::State::Failed:
            removeAgentActivity();
            break;
        default:
            break;
    }
    updateClearButton();
}

void MainWindow::loadMcpServers()
{
    mcpConfigurations_ = settingsStore_.mcpServerConfigs();
    for (const auto& config : mcpConfigurations_)
    {
        if (config.serverId.trimmed() == QLatin1String("filesystem"))
            filesystemConfiguredExternally_ = true;
        if (!config.enabled) continue;
        QString errorMessage;
        if (!mcpManager_.addServer(config, errorMessage))
        {
            const auto safeMessage = redactSensitiveText(errorMessage);
            mcpServerErrors_.insert(config.serverId, safeMessage);
            qWarning().noquote() << "MCP config rejected:" << safeMessage;
            continue;
        }
        mcpManager_.startServer(config.serverId);
    }

    if (!filesystemConfiguredExternally_ &&
        settingsStore_.builtInFilesystemMcpEnabled())
    {
        QString errorMessage;
        if (!startBuiltInFilesystem(workspacePath_, errorMessage))
        {
            const auto safeMessage = redactSensitiveText(errorMessage);
            mcpServerErrors_.insert(QStringLiteral("filesystem"), safeMessage);
            qWarning().noquote()
                << "Built-in filesystem MCP unavailable:" << safeMessage;
        }
    }
    refreshMcpServerMenu();
}

void MainWindow::refreshMcpServerMenu()
{
    const auto detailFor = [this](const QString& serverId, bool enabled)
    {
        if (!enabled) return tr("Off");
        const auto snapshot = mcpManager_.serverSnapshot(serverId);
        if (!snapshot) return tr("Unavailable");
        using infrastructure::mcp::McpServerState;
        switch (snapshot->state)
        {
            case McpServerState::Stopped:
                return tr("Stopped");
            case McpServerState::Starting:
                return tr("Starting");
            case McpServerState::Initializing:
                return tr("Initializing");
            case McpServerState::Ready:
                return tr("%n tools", nullptr,
                          static_cast<int>(snapshot->toolCount));
            case McpServerState::Degraded:
                return tr("Degraded - %n tools", nullptr,
                          static_cast<int>(snapshot->toolCount));
            case McpServerState::Failed:
                return tr("Failed");
            case McpServerState::Stopping:
                return tr("Stopping");
        }
        return tr("Unavailable");
    };

    QList<McpServerPresentation> presentations;
    const auto builtInAvailable = !filesystemConfiguredExternally_;
    const auto builtInEnabled =
        builtInAvailable && settingsStore_.builtInFilesystemMcpEnabled();
    presentations.append(
        {QStringLiteral("builtin-filesystem"), tr("Built-in filesystem"),
         builtInAvailable
             ? detailFor(QStringLiteral("filesystem"), builtInEnabled)
             : tr("Replaced by external filesystem"),
         builtInEnabled, true, builtInAvailable});

    for (const auto& config : mcpConfigurations_)
    {
        presentations.append({config.serverId, config.serverId,
                              detailFor(config.serverId, config.enabled),
                              config.enabled, false, true});
    }
    chatView_->setMcpServers(presentations);
    refreshMcpControlPanel();
}

void MainWindow::refreshMcpControlPanel()
{
    if (mcpControlPanel_ == nullptr) return;

    const auto controlsEnabled =
        !agentRunActive_ && !chatController_.isGenerating();
    const auto allTools = mcpManager_.tools();
    const auto allResources = mcpManager_.resources();
    const auto allTemplates = mcpManager_.resourceTemplates();
    const auto allPrompts = mcpManager_.prompts();
    const auto riskText = [this](infrastructure::mcp::ToolRisk risk)
    {
        switch (risk)
        {
            case infrastructure::mcp::ToolRisk::ReadOnly:
                return tr("read only");
            case infrastructure::mcp::ToolRisk::CreatesData:
                return tr("creates data");
            case infrastructure::mcp::ToolRisk::ModifiesData:
                return tr("modifies data");
            case infrastructure::mcp::ToolRisk::Destructive:
                return tr("destructive");
        }
        return tr("unknown risk");
    };
    const auto policyText = [this, &riskText](const QString& qualifiedName)
    {
        QString decision;
        switch (toolPolicy_.evaluate(qualifiedName))
        {
            case infrastructure::mcp::ToolDecision::Allow:
                decision = tr("Allowed");
                break;
            case infrastructure::mcp::ToolDecision::RequireApproval:
                decision = tr("Approval required");
                break;
            case infrastructure::mcp::ToolDecision::Deny:
                decision = tr("Blocked");
                break;
        }
        return QStringLiteral("%1, %2").arg(
            decision, riskText(toolPolicy_.risk(qualifiedName)));
    };
    const auto hintsText = [this](const QJsonObject& annotations)
    {
        QStringList hints;
        if (annotations.value(QStringLiteral("readOnlyHint")).toBool())
            hints.append(tr("read only"));
        if (annotations.value(QStringLiteral("destructiveHint")).toBool())
            hints.append(tr("destructive"));
        if (annotations.value(QStringLiteral("idempotentHint")).toBool())
            hints.append(tr("idempotent"));
        if (annotations.value(QStringLiteral("openWorldHint")).toBool())
            hints.append(tr("open world"));
        return hints.isEmpty() ? tr("None") : hints.join(QStringLiteral(", "));
    };

    const auto makePresentation =
        [this, controlsEnabled, &allTools, &allResources, &allTemplates,
         &allPrompts, &policyText,
         &hintsText](const infrastructure::mcp::McpServerConfig& config,
                     const QString& displayName, bool builtIn, bool available,
                     bool enabled)
    {
        McpServerControlPresentation presentation;
        presentation.serverId = config.serverId;
        presentation.displayName = displayName;
        presentation.enabled = enabled;
        presentation.builtIn = builtIn;
        presentation.available = available;
        presentation.controlsEnabled = controlsEnabled;
        const auto snapshot = mcpManager_.serverSnapshot(config.serverId);
        presentation.registered = snapshot.has_value();
        if (snapshot) presentation.snapshot = *snapshot;
        presentation.snapshot.serverId = config.serverId;
        if (mcpServerErrors_.contains(config.serverId))
        {
            if (presentation.snapshot.lastErrorCode.isEmpty())
                presentation.snapshot.lastErrorCode =
                    QStringLiteral("host_error");
            presentation.snapshot.lastErrorMessage =
                mcpServerErrors_.value(config.serverId);
        }
        presentation.transport = QStringLiteral("stdio");
        const auto serverName =
            presentation.snapshot.serverInfo.value(QStringLiteral("name"))
                .toString();
        const auto serverVersion =
            presentation.snapshot.serverInfo.value(QStringLiteral("version"))
                .toString();
        presentation.serverInformation =
            serverName.isEmpty() ? tr("Not available")
                                 : QStringLiteral("%1 %2")
                                       .arg(serverName, serverVersion)
                                       .trimmed();
        presentation.program = QDir::toNativeSeparators(config.program);
        presentation.workingDirectory =
            config.workingDirectory.isEmpty()
                ? tr("Process default")
                : QDir::toNativeSeparators(config.workingDirectory);
        presentation.configurationSummary =
            tr("stdio; %n arguments", nullptr, config.arguments.size()) +
            tr("; %n environment variables", nullptr,
               config.environment.size());
        presentation.allowlist =
            config.toolAllowlist.isEmpty()
                ? tr("All advertised tools")
                : config.toolAllowlist.join(QStringLiteral(", "));
        QStringList roots;
        const auto registeredRoots = mcpManager_.roots(config.serverId);
        if (!registeredRoots.isEmpty())
            for (const auto& root : registeredRoots)
                roots.append(QDir::toNativeSeparators(root.canonicalPath));
        else
            for (const auto& root : config.authorizedRoots)
                roots.append(QDir::toNativeSeparators(root));
        presentation.authorizedRoots = roots.isEmpty()
                                           ? tr("None configured by the Host")
                                           : roots.join(QLatin1Char('\n'));
        for (const auto& tool : allTools)
        {
            if (tool.serverId != config.serverId) continue;
            presentation.tools.append({tool.name,
                                       policyText(tool.qualifiedName),
                                       hintsText(tool.annotations)});
        }
        for (const auto& resource : allResources)
            if (resource.serverId == config.serverId)
                presentation.resources.append(
                    {resource.uri, resource.name, resource.mimeType, false,
                     mcpManager_.isResourceSubscribed(config.serverId,
                                                      resource.uri)});
        for (const auto& resourceTemplate : allTemplates)
            if (resourceTemplate.serverId == config.serverId)
                presentation.resources.append(
                    {resourceTemplate.uriTemplate, resourceTemplate.name,
                     resourceTemplate.mimeType, true, false});
        for (const auto& prompt : allPrompts)
            if (prompt.serverId == config.serverId)
                presentation.prompts.append(
                    {prompt.name, prompt.description, prompt.arguments});
        presentation.diagnostics = mcpDiagnostics_.value(config.serverId);
        return presentation;
    };

    QList<McpServerControlPresentation> presentations;
    if (!filesystemConfiguredExternally_)
    {
        infrastructure::mcp::McpServerConfig config;
        config.serverId = QStringLiteral("filesystem");
        config.program = tr("Built-in executable");
        config.workingDirectory = workspacePath_;
        config.arguments = {QStringLiteral("--write-root"), workspacePath_};
        config.authorizedRoots = {workspacePath_};
        presentations.append(
            makePresentation(config, tr("Built-in filesystem"), true, true,
                             settingsStore_.builtInFilesystemMcpEnabled()));
    }
    for (const auto& config : mcpConfigurations_)
        presentations.append(makePresentation(config, config.serverId, false,
                                              true, config.enabled));
    mcpControlPanel_->setServers(presentations);
}

void MainWindow::appendMcpDiagnostic(const QString& serverId,
                                     const QString& category,
                                     const QString& text)
{
    if (serverId.trimmed().isEmpty()) return;
    const auto safeText = redactSensitiveText(text);
    if (safeText.isEmpty()) return;
    auto& diagnostics = mcpDiagnostics_[serverId];
    diagnostics.append(QStringLiteral("[%1] %2: %3")
                           .arg(QDateTime::currentDateTime().toString(
                                    QStringLiteral("HH:mm:ss")),
                                category.isEmpty() ? tr("Event") : category,
                                safeText));
    constexpr auto maximumDiagnostics = 200;
    while (diagnostics.size() > maximumDiagnostics)
        diagnostics.removeFirst();
    refreshMcpControlPanel();
}

void MainWindow::setModelPath(const QString& modelPath)
{
    const auto path = modelPath.trimmed();
    if (path != pendingModelSelection_.selectedPath &&
        path != activeModelSelection_.selectedPath)
        modelInfoText_ = tr("Model not checked");
    modelPath_ = path;
    auto badgeState = modelPath_.isEmpty() ? ModelBadgeState::Neutral
                                           : ModelBadgeState::Unverified;
    if (modelPath_ == activeModelSelection_.selectedPath)
        badgeState = modelBadgeState(activeModelSelection_);
    else if (modelPath_ == pendingModelSelection_.selectedPath)
        badgeState = modelBadgeState(pendingModelSelection_);
    chatView_->setModelPresentation(modelPath_, modelInfoText_, badgeState);

    const auto state = workerClient_.state();
    const auto ready = state == infrastructure::WorkerClient::State::Ready ||
                       state == infrastructure::WorkerClient::State::ModelReady;
    chatView_->setModelControlsEnabled(
        ready && !verifyingModelPackage_,
        ready && !verifyingModelPackage_ && !modelPath_.isEmpty());
}

bool MainWindow::startBuiltInFilesystem(const QString& workspacePath,
                                        QString& errorMessage)
{
    infrastructure::mcp::McpServerConfig config;
    if (!infrastructure::mcp::createBuiltInFilesystemServerConfig(
            QCoreApplication::applicationDirPath(),
            QCoreApplication::applicationVersion(), workspacePath, config,
            errorMessage))
        return false;

    if (builtInFilesystemRunning_)
    {
        if (!mcpManager_.removeServer(QStringLiteral("filesystem"),
                                      errorMessage))
            return false;
        builtInFilesystemRunning_ = false;
    }
    if (!mcpManager_.addServer(config, errorMessage)) return false;
    builtInFilesystemRunning_ = true;
    mcpServerErrors_.remove(QStringLiteral("filesystem"));
    mcpManager_.startServer(config.serverId);
    return true;
}

void MainWindow::beginModelLoad(models::ModelSelection selection)
{
    pendingModelSelection_ = std::move(selection);
    updateModelInformation(pendingModelSelection_);
    replacingModel_ = workerClient_.state() ==
                      infrastructure::WorkerClient::State::ModelReady;
    if (replacingModel_)
    {
        chatView_->setStatusText(tr("Releasing current model..."));
        workerClient_.unloadModel();
        return;
    }

    startPendingModelLoad();
}

void MainWindow::startPendingModelLoad()
{
    chatView_->setStatusText(replacingModel_ ? tr("Loading new model...")
                                             : tr("Loading model..."));
    workerClient_.loadModel(pendingModelSelection_.modelPath);
}

void MainWindow::continueModelLoadAfterUnload()
{
    activeModelSelection_ = {};
    if (!replacingModel_ || pendingModelSelection_.modelPath.isEmpty()) return;
    startPendingModelLoad();
}

void MainWindow::handleModelLoadFailure(const QString& code,
                                        const QString& message)
{
    const auto previousModelWasUnloaded = replacingModel_;
    replacingModel_ = false;
    activeModelSelection_ = {};
    pendingModelSelection_ = {};
    modelInfoText_ = tr("Model load failed");
    chatView_->setModelPresentation(modelPath_, modelInfoText_,
                                    ModelBadgeState::Invalid);

    const auto detail =
        previousModelWasUnloaded
            ? tr("%1 The previous model has already been unloaded; no model "
                 "is currently loaded.")
                  .arg(message)
            : message;
    showError(code, detail);
}

void MainWindow::handleModelUnloadFailure(const QString& code,
                                          const QString& message)
{
    replacingModel_ = false;
    pendingModelSelection_ = {};
    modelPath_ = activeModelSelection_.selectedPath;
    updateModelInformation(activeModelSelection_);
    showError(code,
              tr("Unable to release the current model: %1 The current model "
                 "is still available.")
                  .arg(message));
}

bool MainWindow::modelHashesAreCached(
    const models::ModelSelection& selection) const
{
    const QDir packageDirectory(selection.descriptor.packageDirectory);
    for (const auto& modelFile : selection.descriptor.modelFiles)
    {
        if (!settingsStore_.isModelFileVerified(
                packageDirectory.filePath(modelFile.path), modelFile.sizeBytes,
                modelFile.sha256))
            return false;
    }
    return !selection.descriptor.modelFiles.isEmpty();
}

bool MainWindow::cacheVerifiedModel(
    const models::ModelSelection& selection) const
{
    const QDir packageDirectory(selection.descriptor.packageDirectory);
    auto saved = true;
    for (const auto& modelFile : selection.descriptor.modelFiles)
    {
        saved = settingsStore_.setModelFileVerified(
                    packageDirectory.filePath(modelFile.path),
                    modelFile.sizeBytes, modelFile.sha256) &&
                saved;
    }
    return saved;
}

void MainWindow::updateModelInformation(const models::ModelSelection& selection)
{
    qint64 totalSize = 0;
    for (const auto& modelFile : selection.descriptor.modelFiles)
        totalSize += modelFile.sizeBytes;
    const auto sizeGiB =
        static_cast<double>(totalSize) / (1024.0 * 1024.0 * 1024.0);

    if (selection.descriptor.origin == models::ModelOrigin::DirectGguf)
    {
        modelInfoText_ = tr("Unverified GGUF - %1 - %2 GiB")
                             .arg(selection.descriptor.displayName)
                             .arg(sizeGiB, 0, 'f', 2);
        chatView_->setModelPresentation(modelPath_, modelInfoText_,
                                        ModelBadgeState::Unverified);
        return;
    }

    const auto verification = selection.descriptor.verificationStatus ==
                                      models::VerificationStatus::Verified
                                  ? tr("Verified package")
                                  : tr("Package not yet verified");
    modelInfoText_ = tr("%1 - %2 - %3 GiB - %4 GB RAM - %5 token context")
                         .arg(verification, selection.descriptor.displayName)
                         .arg(sizeGiB, 0, 'f', 2)
                         .arg(selection.descriptor.recommendedRamGb)
                         .arg(selection.preset.contextSize);
    chatView_->setModelPresentation(modelPath_, modelInfoText_,
                                    modelBadgeState(selection));
}
}  // namespace qtllm::ui
