#include "MainWindow.hpp"

#include <QCoreApplication>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTextCursor>
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

    connect(&workerClient_, &infrastructure::WorkerClient::stateChanged, this,
            &MainWindow::updateState);
    connect(&workerClient_, &infrastructure::WorkerClient::modelLoaded, this,
            [this](const QString& path, qint64 milliseconds)
            {
                statusLabel_->setText(
                    tr("Model ready - loaded in %1 ms").arg(milliseconds));
                modelPathEdit_->setText(path);
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

    appendMessage(tr("You"), prompt);
    transcript_->appendPlainText(tr("Assistant"));
    promptEdit_->clear();
    pendingUtf8_.clear();
    workerClient_.generate(prompt,
                           QStringLiteral("You are a helpful assistant."));
}

void MainWindow::stopGeneration()
{
    workerClient_.cancel();
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
    transcript_->appendPlainText(QStringLiteral("\n"));
    statusLabel_->setText(
        cancelled ? tr("Generation stopped")
                  : tr("Ready - %1 token/s")
                        .arg(metrics.value(QStringLiteral("tokensPerSecond"))
                                 .toDouble(),
                             0, 'f', 1));
}

void MainWindow::showError(const QString& code, const QString& message)
{
    statusLabel_->setText(tr("Error: %1").arg(message));
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

    transcript_ = new QPlainTextEdit(central);
    transcript_->setReadOnly(true);
    transcript_->setPlaceholderText(tr("Conversation"));
    transcript_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(transcript_, 1);

    promptEdit_ = new QPlainTextEdit(central);
    promptEdit_->setPlaceholderText(tr("Message"));
    promptEdit_->setMaximumHeight(110);
    layout->addWidget(promptEdit_);

    auto* actionRow = new QHBoxLayout;
    actionRow->addStretch();
    stopButton_ = new QPushButton(tr("Stop"), central);
    sendButton_ = new QPushButton(tr("Send"), central);
    sendButton_->setDefault(true);
    actionRow->addWidget(stopButton_);
    actionRow->addWidget(sendButton_);
    layout->addLayout(actionRow);

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

void MainWindow::appendMessage(const QString& role, const QString& text)
{
    if (!transcript_->document()->isEmpty())
        transcript_->appendPlainText(QString());
    transcript_->appendPlainText(role);
    transcript_->appendPlainText(text);
    transcript_->appendPlainText(QString());
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

    auto cursor = transcript_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QString::fromUtf8(pendingUtf8_.constData(), validLength));
    transcript_->setTextCursor(cursor);
    transcript_->ensureCursorVisible();
    pendingUtf8_.remove(0, validLength);
}
}  // namespace qtllm::ui
