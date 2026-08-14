#include "MainWindow.hpp"

#include "AssistantResponse.hpp"
#include "MessageWidget.hpp"

#include <QCoreApplication>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace qtllm::ui
{
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(tr("qtLLM"));
    resize(980, 700);
    setMinimumSize(720, 520);

    buildUi();
    modelPathEdit_->setText(settingsStore_.lastModelPath());

    connect(&workerClient_, &infrastructure::WorkerClient::stateChanged, this,
            &MainWindow::updateState);
    connect(
        &workerClient_, &infrastructure::WorkerClient::modelLoaded, this,
        [this](const QString& path, qint64 milliseconds, const QString& device)
        {
            statusLabel_->setText(
                tr("Model ready on %1 - loaded in %2 ms")
                    .arg(device.isEmpty() ? tr("CPU") : device)
                    .arg(milliseconds));
            modelPathEdit_->setText(path);
            if (!settingsStore_.setLastModelPath(path))
            {
                statusLabel_->setText(
                    tr("Model ready, but the model path could not be "
                       "saved beside the application."));
            }
        });
    connect(&workerClient_, &infrastructure::WorkerClient::tokenReceived, this,
            &MainWindow::appendToken);
    connect(&workerClient_, &infrastructure::WorkerClient::generationFinished,
            this, &MainWindow::finishGeneration);
    connect(&workerClient_, &infrastructure::WorkerClient::errorOccurred, this,
            &MainWindow::showError);
    connect(&workerClient_, &infrastructure::WorkerClient::diagnosticReceived,
            this,
            [](const QString& text) { qInfo().noquote() << text.trimmed(); });

    workerClient_.start();
}

void MainWindow::selectModel()
{
    const auto selected = QFileDialog::getOpenFileName(
        this, tr("Select GGUF model"), modelPathEdit_->text(),
        tr("GGUF models (*.gguf);;All files (*)"));
    if (!selected.isEmpty()) modelPathEdit_->setText(selected);
}

void MainWindow::loadSelectedModel()
{
    workerClient_.loadModel(modelPathEdit_->text().trimmed());
}

void MainWindow::sendPrompt()
{
    const auto prompt = promptEdit_->toPlainText().trimmed();
    if (prompt.isEmpty()) return;

    appendUserMessage(prompt);
    beginAssistantMessage();
    promptEdit_->clear();
    pendingUtf8_.clear();
    currentAssistantText_.clear();
    conversationMessages_.append({chat::Role::User, prompt});
    hasPendingHistoryMessage_ = true;

    auto requestMessages = conversationMessages_;
    requestMessages.prepend(
        {chat::Role::System, QStringLiteral("You are a helpful assistant.")});
    workerClient_.generate(requestMessages);
}

void MainWindow::stopGeneration()
{
    workerClient_.cancel();
}

void MainWindow::clearConversation()
{
    if (workerClient_.state() ==
        infrastructure::WorkerClient::State::Generating)
        return;

    while (conversationLayout_->count() > 1)
    {
        auto* item = conversationLayout_->takeAt(0);
        delete item->widget();
        delete item;
    }

    conversationMessages_.clear();
    rawTranscript_->clear();
    renderTimer_->stop();
    currentAssistant_ = nullptr;
    currentAssistantText_.clear();
    pendingUtf8_.clear();
    hasPendingHistoryMessage_ = false;
    statusLabel_->setText(tr("Conversation cleared"));
    updateClearButton();
}

void MainWindow::updateState(infrastructure::WorkerClient::State state)
{
    const auto ready = state == infrastructure::WorkerClient::State::Ready ||
                       state == infrastructure::WorkerClient::State::ModelReady;
    const auto modelReady =
        state == infrastructure::WorkerClient::State::ModelReady;
    const auto generating =
        state == infrastructure::WorkerClient::State::Generating;

    modelPathEdit_->setEnabled(ready);
    browseButton_->setEnabled(ready);
    loadButton_->setEnabled(ready &&
                            !modelPathEdit_->text().trimmed().isEmpty());
    promptEdit_->setEnabled(modelReady);
    sendButton_->setEnabled(modelReady);
    stopButton_->setEnabled(generating);
    updateClearButton();

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
    appendRawText(QStringLiteral("\n"));
    currentAssistant_ = nullptr;

    if (!cancelled)
    {
        const auto answer = assistantHistoryText(currentAssistantText_);
        if (!answer.isEmpty())
        {
            conversationMessages_.append({chat::Role::Assistant, answer});
            hasPendingHistoryMessage_ = false;
        }
        else
        {
            discardPendingHistoryMessage();
        }
    }
    else
    {
        discardPendingHistoryMessage();
    }

    if (cancelled)
    {
        statusLabel_->setText(tr("Generation stopped"));
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
}

void MainWindow::showError(const QString& code, const QString& message)
{
    QString retryPrompt;
    if (hasPendingHistoryMessage_ && !conversationMessages_.isEmpty() &&
        conversationMessages_.constLast().role == chat::Role::User)
    {
        retryPrompt = conversationMessages_.constLast().content;
    }
    statusLabel_->setText(tr("Error: %1").arg(message));
    if (currentAssistant_ != nullptr)
    {
        currentAssistantText_ +=
            QStringLiteral("\n\n**%1:** %2").arg(tr("Error"), message);
        appendRawText(QStringLiteral("\nError: %1\n").arg(message));
        renderTimer_->stop();
        renderAssistant(true);
        currentAssistant_ = nullptr;
    }
    discardPendingHistoryMessage();
    if (!retryPrompt.isEmpty() && promptEdit_->toPlainText().isEmpty())
    {
        promptEdit_->setPlainText(retryPrompt);
        promptEdit_->setFocus();
    }
    qWarning().noquote() << code << message;
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(10);

    auto* modelRow = new QHBoxLayout;
    modelPathEdit_ = new QLineEdit(central);
    modelPathEdit_->setPlaceholderText(tr("Local GGUF model path"));
    browseButton_ = new QPushButton(tr("Browse"), central);
    loadButton_ = new QPushButton(tr("Load"), central);
    browseButton_->setToolTip(tr("Select local GGUF model"));
    modelRow->addWidget(modelPathEdit_, 1);
    modelRow->addWidget(browseButton_);
    modelRow->addWidget(loadButton_);
    layout->addLayout(modelRow);

    transcriptTabs_ = new QTabWidget(central);
    conversationScroll_ = new QScrollArea(transcriptTabs_);
    conversationScroll_->setWidgetResizable(true);
    conversationScroll_->setFrameShape(QFrame::NoFrame);
    auto* conversationContent = new QWidget(conversationScroll_);
    conversationLayout_ = new QVBoxLayout(conversationContent);
    conversationLayout_->setContentsMargins(6, 6, 6, 6);
    conversationLayout_->setSpacing(8);
    conversationLayout_->addStretch();
    conversationScroll_->setWidget(conversationContent);

    rawTranscript_ = new QPlainTextEdit(transcriptTabs_);
    rawTranscript_->setObjectName(QStringLiteral("rawTranscript"));
    rawTranscript_->setReadOnly(true);
    rawTranscript_->setFont(
        QFontDatabase::systemFont(QFontDatabase::FixedFont));

    transcriptTabs_->addTab(conversationScroll_, tr("Conversation"));
    transcriptTabs_->addTab(rawTranscript_, tr("Raw"));
    layout->addWidget(transcriptTabs_, 1);

    promptEdit_ = new QPlainTextEdit(central);
    promptEdit_->setObjectName(QStringLiteral("promptEditor"));
    promptEdit_->setPlaceholderText(tr("Message"));
    promptEdit_->setMaximumHeight(110);
    layout->addWidget(promptEdit_);

    auto* actionRow = new QHBoxLayout;
    clearButton_ = new QPushButton(tr("Clear"), central);
    clearButton_->setObjectName(QStringLiteral("clearConversationButton"));
    clearButton_->setToolTip(tr("Clear the current conversation"));
    actionRow->addWidget(clearButton_);
    actionRow->addStretch();
    stopButton_ = new QPushButton(tr("Stop"), central);
    sendButton_ = new QPushButton(tr("Send"), central);
    sendButton_->setDefault(true);
    actionRow->addWidget(stopButton_);
    actionRow->addWidget(sendButton_);
    layout->addLayout(actionRow);

    renderTimer_ = new QTimer(this);
    renderTimer_->setSingleShot(true);
    renderTimer_->setInterval(40);
    connect(renderTimer_, &QTimer::timeout, this,
            [this] { renderAssistant(); });

    statusLabel_ = new QLabel(tr("Starting worker..."), this);
    statusBar()->addWidget(statusLabel_, 1);
    setCentralWidget(central);

    connect(browseButton_, &QPushButton::clicked, this,
            &MainWindow::selectModel);
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

    if (!rawTranscript_->document()->isEmpty())
        appendRawText(QStringLiteral("\n"));
    appendRawText(tr("You") + QStringLiteral("\n") + text +
                  QStringLiteral("\n\n"));
    updateClearButton();
    scrollConversationToBottom();
}

void MainWindow::beginAssistantMessage()
{
    currentAssistant_ = new MessageWidget(MessageWidget::Role::Assistant);
    currentAssistant_->setAssistantText({}, false);
    conversationLayout_->insertWidget(conversationLayout_->count() - 1,
                                      currentAssistant_);
    appendRawText(tr("Assistant") + QStringLiteral("\n"));
    scrollConversationToBottom();
}

void MainWindow::appendRawText(const QString& text)
{
    auto cursor = rawTranscript_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    rawTranscript_->setTextCursor(cursor);
    rawTranscript_->ensureCursorVisible();
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
    appendRawText(text);
    if (!final && !renderTimer_->isActive()) renderTimer_->start();
}

void MainWindow::discardPendingHistoryMessage()
{
    if (!hasPendingHistoryMessage_) return;
    if (!conversationMessages_.isEmpty() &&
        conversationMessages_.constLast().role == chat::Role::User)
    {
        conversationMessages_.removeLast();
    }
    hasPendingHistoryMessage_ = false;
}

void MainWindow::updateClearButton()
{
    if (clearButton_ == nullptr || conversationLayout_ == nullptr) return;
    const auto generating = workerClient_.state() ==
                            infrastructure::WorkerClient::State::Generating;
    clearButton_->setEnabled(!generating && conversationLayout_->count() > 1);
}
}  // namespace qtllm::ui
