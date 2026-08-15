#include "MainWindow.hpp"

#include "BuiltInMcpServer.hpp"
#include "MessageWidget.hpp"
#include "SensitiveData.hpp"
#include "ToolApprovalWidget.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace qtllm::ui
{
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      mcpManager_(this),
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
    setWindowTitle(tr("qtLLM"));
    resize(1080, 760);
    setMinimumSize(720, 520);

    buildUi();
    modelPathEdit_->setText(settingsStore_.lastModelPath());
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
    updateWorkspaceLink();

    connect(&workerClient_, &infrastructure::WorkerClient::stateChanged, this,
            &MainWindow::updateState);
    connect(
        &workerClient_, &infrastructure::WorkerClient::modelLoaded, this,
        [this](const QString& path, qint64 milliseconds, const QString& device)
        {
            Q_UNUSED(path)
            activeModelSelection_ = pendingModelSelection_;
            statusLabel_->setText(
                tr("Model ready on %1 - loaded in %2 ms")
                    .arg(device.isEmpty() ? tr("CPU") : device)
                    .arg(milliseconds));
            modelPathEdit_->setText(activeModelSelection_.selectedPath);
            updateModelInformation(activeModelSelection_);
            if (!settingsStore_.setLastModelPath(
                    activeModelSelection_.selectedPath))
            {
                statusLabel_->setText(
                    tr("Model ready, but the model path could not be "
                       "saved beside the application."));
            }
        });
    connect(&modelVerificationWatcher_,
            &QFutureWatcher<models::ModelPackageResult>::finished, this,
            &MainWindow::finishModelPackageVerification);
    connect(&workerClient_, &infrastructure::WorkerClient::tokenReceived, this,
            [this](const QByteArray& bytes)
            { agentController_.receiveToken(bytes); });
    connect(&workerClient_, &infrastructure::WorkerClient::generationFinished,
            this, [this](bool cancelled, const QJsonObject& metrics)
            { agentController_.completeGeneration(cancelled, metrics); });
    connect(&workerClient_, &infrastructure::WorkerClient::errorOccurred, this,
            [this](const QString& code, const QString& message)
            { agentController_.handleGenerationError(code, message); });
    connect(&agentController_,
            &application::AgentController::userRequestAccepted, this,
            [this](const QString&, const QString& prompt)
            {
                appendUserMessage(prompt);
                showAgentActivity();
                promptEdit_->clear();
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
                conversationLayout_->insertWidget(
                    conversationLayout_->count() - 1, approval);
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
                    updateAgentActivity(event.message);
                if (!event.message.isEmpty())
                    statusLabel_->setText(event.message);
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
                    statusLabel_->setText(tr("Ready"));
                else if (!message.isEmpty())
                {
                    if (code != QLatin1String("approval_denied") &&
                        promptEdit_->toPlainText().isEmpty() &&
                        agentController_.activeRun().has_value())
                    {
                        promptEdit_->setPlainText(
                            agentController_.activeRun()->userRequest);
                    }
                    showError(code, message);
                }
                updateState(workerClient_.state());
            });
    connect(&mcpManager_, &infrastructure::mcp::McpClientManager::serverStarted,
            this, [this](const QString& serverId)
            { mcpManager_.initialize(serverId); });
    connect(&mcpManager_,
            &infrastructure::mcp::McpClientManager::serverInitialized, this,
            [this](const QString& serverId, const QJsonObject&)
            { mcpManager_.listTools(serverId); });
    connect(&mcpManager_, &infrastructure::mcp::McpClientManager::toolsChanged,
            this,
            [this](const QString&, const QList<agent::ToolDefinition>& tools)
            {
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
            });
    connect(&mcpManager_, &infrastructure::mcp::McpClientManager::serverError,
            this,
            [this](const QString& serverId, const QString& code,
                   const QString& message)
            { qWarning().noquote() << serverId << code << message; });
    connect(
        &mcpManager_, &infrastructure::mcp::McpClientManager::toolResultReady,
        &agentController_, &application::AgentController::receiveToolResult);
    connect(&agentController_,
            &application::AgentController::conversationCleared, this,
            &MainWindow::resetConversationView);
    loadMcpServers();
    workerClient_.start();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == workspacePathLabel_ && event->type() == QEvent::Resize)
        updateWorkspaceLink();

    if (watched == promptEdit_ && event->type() == QEvent::KeyPress)
    {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        const auto isEnter = keyEvent->key() == Qt::Key_Return ||
                             keyEvent->key() == Qt::Key_Enter;
        if (isEnter && !(keyEvent->modifiers() & Qt::ShiftModifier))
        {
            if (sendButton_->isEnabled()) sendPrompt();
            return true;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::selectModelPackage()
{
    const QFileInfo current(modelPathEdit_->text());
    const auto initialDirectory =
        current.isDir() ? current.absoluteFilePath() : current.absolutePath();
    const auto selected = QFileDialog::getExistingDirectory(
        this, tr("Select model package"), initialDirectory,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!selected.isEmpty()) modelPathEdit_->setText(selected);
}

void MainWindow::selectGgufModel()
{
    const QFileInfo current(modelPathEdit_->text());
    const auto selected = QFileDialog::getOpenFileName(
        this, tr("Select GGUF model"), current.absoluteFilePath(),
        tr("GGUF models (*.gguf);;All files (*)"));
    if (!selected.isEmpty()) modelPathEdit_->setText(selected);
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
    if (!startBuiltInFilesystem(workspacePath, errorMessage))
    {
        showError(QStringLiteral("filesystem_mcp_unavailable"), errorMessage);
        return;
    }
    workspacePath_ = workspacePath;
    updateWorkspaceLink();
    if (!settingsStore_.setWorkspacePath(workspacePath))
        qWarning().noquote()
            << "Unable to persist workspace path:" << workspacePath;
}

void MainWindow::openWorkspaceDirectory()
{
    if (workspacePath_.isEmpty()) return;
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(workspacePath_)))
    {
        statusLabel_->setText(tr("Unable to open the workspace folder"));
        qWarning().noquote()
            << "Unable to open workspace folder:" << workspacePath_;
    }
}

void MainWindow::loadSelectedModel()
{
    if (verifyingModelPackage_) return;

    auto result =
        models::ModelPackage::inspect(modelPathEdit_->text().trimmed(),
                                      QCoreApplication::applicationVersion());
    if (!result.succeeded())
    {
        modelInfoLabel_->setText(tr("Invalid model selection"));
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
    modelInfoLabel_->setText(tr("Verifying package - %1")
                                 .arg(result.selection.descriptor.displayName));
    updateState(workerClient_.state());
    modelVerificationWatcher_.setFuture(QtConcurrent::run(
        [selection = std::move(result.selection)]() mutable
        { return models::ModelPackage::verify(std::move(selection)); }));
}

void MainWindow::finishModelPackageVerification()
{
    auto result = modelVerificationWatcher_.result();
    verifyingModelPackage_ = false;
    if (!result.succeeded())
    {
        pendingModelSelection_ = {};
        modelInfoLabel_->setText(tr("Invalid model package"));
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

void MainWindow::sendPrompt()
{
    const auto prompt = promptEdit_->toPlainText().trimmed();
    if (prompt.isEmpty()) return;
    beginAgentPrompt(prompt);
}

void MainWindow::stopGeneration()
{
    if (agentRunActive_) agentController_.cancel();
}

void MainWindow::clearConversation()
{
    if (agentRunActive_) return;
    agentController_.clearConversation();
}

void MainWindow::resetConversationView()
{
    while (conversationLayout_->count() > 1)
    {
        auto* item = conversationLayout_->takeAt(0);
        delete item->widget();
        delete item;
    }

    activityLog_->clear();
    renderTimer_->stop();
    currentAssistant_ = nullptr;
    agentActivityMessage_ = nullptr;
    pendingToolApproval_ = nullptr;
    currentAssistantText_.clear();
    pendingUtf8_.clear();
    statusLabel_->setText(tr("Conversation cleared"));
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
    modelPathEdit_->setEnabled(ready);
    browseButton_->setEnabled(ready);
    loadButton_->setEnabled(ready &&
                            !modelPathEdit_->text().trimmed().isEmpty());
    workspaceButton_->setEnabled(!agentRunActive_ &&
                                 !filesystemConfiguredExternally_);
    promptEdit_->setEnabled(modelReady && !agentRunActive_);
    sendButton_->setEnabled(modelReady && !agentRunActive_);
    stopButton_->setEnabled(generating || agentRunActive_);
    updateClearButton();

    if (verifyingModelPackage_)
    {
        statusLabel_->setText(tr("Verifying model package..."));
        return;
    }

    switch (state)
    {
        case infrastructure::WorkerClient::State::Stopped:
            statusLabel_->setText(tr("Worker stopped"));
            break;
        case infrastructure::WorkerClient::State::Starting:
            statusLabel_->setText(tr("Starting worker..."));
            break;
        case infrastructure::WorkerClient::State::Ready:
            statusLabel_->setText(tr("Select a local model"));
            break;
        case infrastructure::WorkerClient::State::LoadingModel:
            statusLabel_->setText(tr("Loading model..."));
            break;
        case infrastructure::WorkerClient::State::ModelReady:
            statusLabel_->setText(tr("Model ready"));
            break;
        case infrastructure::WorkerClient::State::Generating:
            statusLabel_->setText(tr("Generating..."));
            break;
        case infrastructure::WorkerClient::State::Failed:
            statusLabel_->setText(tr("Worker unavailable"));
            break;
    }
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
        statusLabel_->setText(tr("Generation stopped"));
        updateClearButton();
        return;
    }

    const auto discardedMessages =
        metrics.value(QStringLiteral("discardedMessages")).toInt();
    statusLabel_->setText(
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

void MainWindow::showError(const QString& code, const QString& message,
                           const QString& retryPrompt)
{
    removeAgentActivity();
    statusLabel_->setText(tr("Error: %1").arg(message));
    if (currentAssistant_ != nullptr)
    {
        currentAssistantText_ +=
            QStringLiteral("\n\n**%1:** %2").arg(tr("Error"), message);
        renderTimer_->stop();
        renderAssistant(true);
        currentAssistant_ = nullptr;
    }
    if (!retryPrompt.isEmpty() && promptEdit_->toPlainText().isEmpty())
    {
        promptEdit_->setPlainText(retryPrompt);
        promptEdit_->setFocus();
    }
    updateClearButton();
    qWarning().noquote() << code << message;
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("centralView"));
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(9);

    auto* modelBar = new QWidget(central);
    modelBar->setObjectName(QStringLiteral("modelBar"));
    auto* modelRow = new QHBoxLayout(modelBar);
    modelRow->setContentsMargins(0, 0, 0, 0);
    modelRow->setSpacing(8);
    modelPathEdit_ = new QLineEdit(central);
    modelPathEdit_->setObjectName(QStringLiteral("modelPathEdit"));
    modelPathEdit_->setPlaceholderText(tr("Model package or local GGUF path"));
    browseButton_ = new QPushButton(tr("Browse"), central);
    loadButton_ = new QPushButton(tr("Load"), central);
    browseButton_->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    loadButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    browseButton_->setToolTip(tr("Select a model package or GGUF file"));
    auto* browseMenu = new QMenu(browseButton_);
    auto* packageAction = browseMenu->addAction(tr("Model package folder"));
    auto* ggufAction = browseMenu->addAction(tr("GGUF file"));
    browseButton_->setMenu(browseMenu);
    modelRow->addWidget(modelPathEdit_, 1);
    modelRow->addWidget(browseButton_);
    modelRow->addWidget(loadButton_);
    layout->addWidget(modelBar);

    modelInfoLabel_ = new QLabel(tr("Model not checked"), central);
    modelInfoLabel_->setObjectName(QStringLiteral("modelInfoLabel"));
    modelInfoLabel_->setWordWrap(true);
    layout->addWidget(modelInfoLabel_);

    transcriptTabs_ = new QTabWidget(central);
    transcriptTabs_->setObjectName(QStringLiteral("transcriptTabs"));
    conversationScroll_ = new QScrollArea(transcriptTabs_);
    conversationScroll_->setObjectName(QStringLiteral("conversationScroll"));
    conversationScroll_->setWidgetResizable(true);
    conversationScroll_->setFrameShape(QFrame::NoFrame);
    auto* conversationContent = new QWidget(conversationScroll_);
    conversationContent->setObjectName(QStringLiteral("conversationContent"));
    conversationLayout_ = new QVBoxLayout(conversationContent);
    conversationLayout_->setContentsMargins(8, 8, 8, 8);
    conversationLayout_->setSpacing(2);
    conversationLayout_->addStretch();
    conversationScroll_->setWidget(conversationContent);

    activityLog_ = new QPlainTextEdit(transcriptTabs_);
    activityLog_->setObjectName(QStringLiteral("activityLog"));
    activityLog_->setReadOnly(true);
    activityLog_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    transcriptTabs_->addTab(conversationScroll_, tr("Conversation"));
    transcriptTabs_->addTab(activityLog_, tr("Activity"));
    layout->addWidget(transcriptTabs_, 1);

    auto* promptComposer = new QWidget(central);
    promptComposer->setObjectName(QStringLiteral("promptComposer"));
    auto* promptLayout = new QVBoxLayout(promptComposer);
    promptLayout->setContentsMargins(4, 4, 4, 4);
    promptLayout->setSpacing(0);

    promptEdit_ = new QPlainTextEdit(promptComposer);
    promptEdit_->setObjectName(QStringLiteral("promptEditor"));
    promptEdit_->setPlaceholderText(tr("Write a message"));
    promptEdit_->setMinimumHeight(64);
    promptEdit_->setMaximumHeight(130);
    promptEdit_->installEventFilter(this);
    promptLayout->addWidget(promptEdit_);

    auto* actionRow = new QHBoxLayout;
    actionRow->setContentsMargins(6, 2, 4, 4);
    actionRow->setSpacing(8);
    clearButton_ = new QPushButton(tr("Clear"), promptComposer);
    clearButton_->setObjectName(QStringLiteral("clearConversationButton"));
    clearButton_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    clearButton_->setToolTip(tr("Clear the current conversation"));
    actionRow->addWidget(clearButton_);
    workspaceButton_ = new QToolButton(promptComposer);
    workspaceButton_->setObjectName(QStringLiteral("workspaceBrowseButton"));
    workspaceButton_->setAutoRaise(true);
    workspaceButton_->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    workspaceButton_->setToolTip(tr("Select the workspace folder"));
    actionRow->addWidget(workspaceButton_);
    workspacePathLabel_ = new QLabel(promptComposer);
    workspacePathLabel_->setObjectName(QStringLiteral("workspacePathLink"));
    workspacePathLabel_->setTextFormat(Qt::RichText);
    workspacePathLabel_->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                                 Qt::LinksAccessibleByKeyboard);
    workspacePathLabel_->setToolTip(tr("Open the workspace folder"));
    workspacePathLabel_->setSizePolicy(QSizePolicy::Ignored,
                                       QSizePolicy::Preferred);
    workspacePathLabel_->installEventFilter(this);
    actionRow->addWidget(workspacePathLabel_, 1);
    stopButton_ = new QPushButton(tr("Stop"), promptComposer);
    stopButton_->setObjectName(QStringLiteral("stopButton"));
    stopButton_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    sendButton_ = new QPushButton(tr("Send"), promptComposer);
    sendButton_->setObjectName(QStringLiteral("primaryActionButton"));
    sendButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    sendButton_->setDefault(true);
    actionRow->addWidget(stopButton_);
    actionRow->addWidget(sendButton_);
    promptLayout->addLayout(actionRow);
    layout->addWidget(promptComposer);

    renderTimer_ = new QTimer(this);
    renderTimer_->setSingleShot(true);
    renderTimer_->setInterval(40);
    connect(renderTimer_, &QTimer::timeout, this,
            [this] { renderAssistant(); });

    statusLabel_ = new QLabel(tr("Starting worker..."), this);
    statusBar()->addWidget(statusLabel_, 1);
    setCentralWidget(central);

    connect(packageAction, &QAction::triggered, this,
            &MainWindow::selectModelPackage);
    connect(ggufAction, &QAction::triggered, this,
            &MainWindow::selectGgufModel);
    connect(workspaceButton_, &QToolButton::clicked, this,
            &MainWindow::selectWorkspaceDirectory);
    connect(workspacePathLabel_, &QLabel::linkActivated, this,
            &MainWindow::openWorkspaceDirectory);
    connect(loadButton_, &QPushButton::clicked, this,
            &MainWindow::loadSelectedModel);
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::sendPrompt);
    connect(stopButton_, &QPushButton::clicked, this,
            &MainWindow::stopGeneration);
    connect(clearButton_, &QPushButton::clicked, this,
            &MainWindow::clearConversation);
    connect(modelPathEdit_, &QLineEdit::textChanged, this,
            [this]
            {
                const auto path = modelPathEdit_->text().trimmed();
                if (path != pendingModelSelection_.selectedPath &&
                    path != activeModelSelection_.selectedPath)
                    modelInfoLabel_->setText(tr("Model not checked"));
                loadButton_->setEnabled(
                    (workerClient_.state() ==
                         infrastructure::WorkerClient::State::Ready ||
                     workerClient_.state() ==
                         infrastructure::WorkerClient::State::ModelReady) &&
                    !modelPathEdit_->text().trimmed().isEmpty());
            });
    updateState(infrastructure::WorkerClient::State::Stopped);
}

void MainWindow::appendUserMessage(const QString& text)
{
    auto* message = new MessageWidget(MessageWidget::Role::User);
    message->setUserText(text);
    conversationLayout_->insertWidget(conversationLayout_->count() - 1,
                                      message);

    updateClearButton();
    scrollConversationToBottom();
}

void MainWindow::beginAssistantMessage()
{
    currentAssistant_ = new MessageWidget(MessageWidget::Role::Assistant);
    currentAssistant_->setAssistantText({}, false);
    conversationLayout_->insertWidget(conversationLayout_->count() - 1,
                                      currentAssistant_);
    scrollConversationToBottom();
}

void MainWindow::appendActivityText(const QString& text)
{
    auto cursor = activityLog_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    activityLog_->setTextCursor(cursor);
    activityLog_->ensureCursorVisible();
}

void MainWindow::appendAgentEvent(const agent::Event& event)
{
    QStringList lines;
    if (event.type == agent::EventType::RunStarted)
    {
        if (!activityLog_->document()->isEmpty()) lines.append(QString{});
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
    const auto* scrollBar = conversationScroll_->verticalScrollBar();
    return scrollBar->maximum() - scrollBar->value() <= 48;
}

void MainWindow::scrollConversationToBottom()
{
    QTimer::singleShot(0, conversationScroll_,
                       [this]
                       {
                           auto* scrollBar =
                               conversationScroll_->verticalScrollBar();
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
    if (clearButton_ == nullptr || conversationLayout_ == nullptr) return;
    const auto hasVisibleMessages = conversationLayout_->count() > 1;
    clearButton_->setEnabled(
        !agentRunActive_ &&
        (agentController_.hasConversation() || hasVisibleMessages));
}

void MainWindow::beginAgentPrompt(const QString& prompt)
{
    if (agentRunActive_) return;
    agentRunActive_ = true;
    updateState(workerClient_.state());
    if (!agentController_.start(prompt, activeModelSelection_.preset,
                                mcpManager_.tools()))
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
    conversationLayout_->insertWidget(conversationLayout_->count() - 1,
                                      message);
    updateClearButton();
    scrollConversationToBottom();
}

void MainWindow::showAgentActivity()
{
    if (agentActivityMessage_ != nullptr) return;
    agentActivityMessage_ = new MessageWidget(MessageWidget::Role::Assistant);
    agentActivityMessage_->setProperty("agentActivity", true);
    agentActivityMessage_->setAssistantText(tr("Thinking..."), false);
    conversationLayout_->insertWidget(conversationLayout_->count() - 1,
                                      agentActivityMessage_);
    scrollConversationToBottom();
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
    if (agentActivityMessage_ == nullptr) return;
    conversationLayout_->removeWidget(agentActivityMessage_);
    delete agentActivityMessage_;
    agentActivityMessage_ = nullptr;
}

void MainWindow::updateAgentState(application::AgentRun::State state)
{
    switch (state)
    {
        case application::AgentRun::State::Deciding:
            statusLabel_->setText(tr("Thinking..."));
            updateAgentActivity(tr("Thinking..."));
            break;
        case application::AgentRun::State::WaitingForApproval:
            statusLabel_->setText(tr("Waiting for tool approval"));
            updateAgentActivity(tr("Waiting for tool approval..."));
            break;
        case application::AgentRun::State::ExecutingTool:
            statusLabel_->setText(tr("Using a tool..."));
            updateAgentActivity(tr("Using a tool..."));
            break;
        case application::AgentRun::State::GeneratingAnswer:
            statusLabel_->setText(tr("Preparing the answer..."));
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
    for (const auto& config : settingsStore_.mcpServerConfigs())
    {
        if (config.serverId.trimmed() == QLatin1String("filesystem"))
            filesystemConfiguredExternally_ = true;
        if (!config.enabled) continue;
        QString errorMessage;
        if (!mcpManager_.addServer(config, errorMessage))
        {
            qWarning().noquote() << "MCP config rejected:" << errorMessage;
            continue;
        }
        mcpManager_.startServer(config.serverId);
    }

    if (filesystemConfiguredExternally_) return;
    QString errorMessage;
    if (!startBuiltInFilesystem(workspacePath_, errorMessage))
        qWarning().noquote()
            << "Built-in filesystem MCP unavailable:" << errorMessage;
}

void MainWindow::updateWorkspaceLink()
{
    if (workspacePathLabel_ == nullptr || workspacePath_.isEmpty()) return;

    const auto nativePath = QDir::toNativeSeparators(workspacePath_);
    const auto availableWidth = qMax(80, workspacePathLabel_->width() - 4);
    const auto displayPath = workspacePathLabel_->fontMetrics().elidedText(
        nativePath, Qt::ElideMiddle, availableWidth);
    const auto href = QUrl::fromLocalFile(workspacePath_)
                          .toString(QUrl::FullyEncoded)
                          .toHtmlEscaped();
    workspacePathLabel_->setText(
        QStringLiteral("<a style=\"color:#2563eb;text-decoration:none\" "
                       "href=\"%1\">%2</a>")
            .arg(href, displayPath.toHtmlEscaped()));
    workspacePathLabel_->setToolTip(
        tr("Open workspace folder: %1").arg(nativePath));
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
    mcpManager_.startServer(config.serverId);
    return true;
}

void MainWindow::beginModelLoad(models::ModelSelection selection)
{
    pendingModelSelection_ = std::move(selection);
    updateModelInformation(pendingModelSelection_);
    updateState(workerClient_.state());
    workerClient_.loadModel(pendingModelSelection_.modelPath);
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
        modelInfoLabel_->setText(tr("Unverified GGUF - %1 - %2 GiB")
                                     .arg(selection.descriptor.displayName)
                                     .arg(sizeGiB, 0, 'f', 2));
        return;
    }

    const auto verification = selection.descriptor.verificationStatus ==
                                      models::VerificationStatus::Verified
                                  ? tr("Verified package")
                                  : tr("Package not yet verified");
    modelInfoLabel_->setText(
        tr("%1 - %2 - %3 GiB - %4 GB RAM - %5 token context")
            .arg(verification, selection.descriptor.displayName)
            .arg(sizeGiB, 0, 'f', 2)
            .arg(selection.descriptor.recommendedRamGb)
            .arg(selection.preset.contextSize));
}
}  // namespace qtllm::ui
