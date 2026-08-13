#include "WorkerClient.hpp"

#include "ProtocolVersion.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>
#include <QUuid>

namespace qtllm::infrastructure
{
WorkerClient::WorkerClient(QObject* parent) : QObject(parent)
{
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process_, &QProcess::started, this, &WorkerClient::onStarted);
    connect(&process_, &QProcess::readyReadStandardOutput, this,
            &WorkerClient::onReadyReadStandardOutput);
    connect(&process_, &QProcess::readyReadStandardError, this,
            &WorkerClient::onReadyReadStandardError);
    connect(&process_, &QProcess::finished, this, &WorkerClient::onFinished);
    connect(&process_, &QProcess::errorOccurred, this,
            &WorkerClient::onProcessError);
}

WorkerClient::~WorkerClient()
{
    stop();
}

void WorkerClient::start(const QString& workerPath)
{
    if (process_.state() != QProcess::NotRunning) return;

    const auto executable =
        workerPath.isEmpty() ? defaultWorkerPath() : workerPath;
    if (!QFileInfo::exists(executable))
    {
        fail(QStringLiteral("worker_not_found"),
             QStringLiteral("Worker executable does not exist: %1")
                 .arg(executable));
        return;
    }

    stopping_ = false;
    standardOutputBuffer_.clear();
    setState(State::Starting);
    process_.setProgram(executable);
    process_.setArguments({QStringLiteral("--ipc")});
    process_.start();
}

void WorkerClient::stop()
{
    if (process_.state() == QProcess::NotRunning) return;
    stopping_ = true;
    process_.closeWriteChannel();
    if (!process_.waitForFinished(3000))
    {
        process_.kill();
        process_.waitForFinished(1000);
    }
}

void WorkerClient::loadModel(const QString& modelPath, int gpuLayers)
{
    if (state_ != State::Ready && state_ != State::ModelReady)
    {
        fail(QStringLiteral("invalid_state"),
             QStringLiteral("Worker is not ready to load a model."));
        return;
    }
    setState(State::LoadingModel);
    loadRequestId_ =
        send(QString::fromLatin1(protocol::message_type::loadModel),
             {{QStringLiteral("modelPath"), modelPath},
              {QStringLiteral("gpuLayers"), gpuLayers}});
}

void WorkerClient::unloadModel()
{
    if (state_ != State::ModelReady) return;
    unloadRequestId_ =
        send(QString::fromLatin1(protocol::message_type::unloadModel));
}

void WorkerClient::generate(const QString& prompt, const QString& systemPrompt,
                            int contextSize, int maxTokens, int threads,
                            float temperature, float topP, int topK,
                            float repeatPenalty)
{
    QList<chat::Message> messages;
    if (!systemPrompt.isEmpty())
        messages.append({chat::Role::System, systemPrompt});
    messages.append({chat::Role::User, prompt});
    generate(messages, contextSize, maxTokens, threads, temperature, topP, topK,
             repeatPenalty);
}

void WorkerClient::generate(const QList<chat::Message>& messages,
                            int contextSize, int maxTokens, int threads,
                            float temperature, float topP, int topK,
                            float repeatPenalty)
{
    if (state_ != State::ModelReady)
    {
        fail(QStringLiteral("invalid_state"),
             QStringLiteral("Load a model before generating."));
        return;
    }
    if (threads <= 0) threads = qMax(1, QThread::idealThreadCount());

    QJsonArray serializedMessages;
    for (const auto& message : messages)
    {
        serializedMessages.append(
            QJsonObject{{QStringLiteral("role"), chat::roleName(message.role)},
                        {QStringLiteral("content"), message.content}});
    }
    generationRequestId_ =
        send(QString::fromLatin1(protocol::message_type::generate),
             {{QStringLiteral("messages"), serializedMessages},
              {QStringLiteral("contextSize"), contextSize},
              {QStringLiteral("maxTokens"), maxTokens},
              {QStringLiteral("threads"), threads},
              {QStringLiteral("temperature"), temperature},
              {QStringLiteral("topP"), topP},
              {QStringLiteral("topK"), topK},
              {QStringLiteral("repeatPenalty"), repeatPenalty}});
    setState(State::Generating);
}

void WorkerClient::cancel()
{
    if (state_ != State::Generating || generationRequestId_.isEmpty()) return;
    send(QString::fromLatin1(protocol::message_type::cancel),
         {{QStringLiteral("targetRequestId"), generationRequestId_}});
}

WorkerClient::State WorkerClient::state() const
{
    return state_;
}

QString WorkerClient::modelPath() const
{
    return modelPath_;
}

QString WorkerClient::activeGenerationRequestId() const
{
    return generationRequestId_;
}

void WorkerClient::onStarted()
{
    helloRequestId_ =
        send(QString::fromLatin1(protocol::message_type::hello),
             {{QStringLiteral("clientVersion"), QStringLiteral("0.1.0")}});
}

void WorkerClient::onReadyReadStandardOutput()
{
    standardOutputBuffer_ += process_.readAllStandardOutput();
    while (true)
    {
        const auto newline = standardOutputBuffer_.indexOf('\n');
        if (newline < 0) break;
        const auto line = standardOutputBuffer_.left(newline);
        standardOutputBuffer_.remove(0, newline + 1);
        if (line.trimmed().isEmpty()) continue;

        protocol::Message message;
        QString errorMessage;
        if (!protocol::decodeLine(line, message, errorMessage))
        {
            fail(QStringLiteral("invalid_worker_message"), errorMessage);
            continue;
        }
        if (message.protocolVersion != protocol::version)
        {
            fail(QStringLiteral("protocol_mismatch"),
                 QStringLiteral("Worker protocol version %1 is not supported.")
                     .arg(message.protocolVersion));
            continue;
        }
        handleMessage(message);
    }
}

void WorkerClient::onReadyReadStandardError()
{
    const auto text = QString::fromUtf8(process_.readAllStandardError());
    if (!text.isEmpty()) emit diagnosticReceived(text);
}

void WorkerClient::onFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    modelPath_.clear();
    generationRequestId_.clear();
    if (stopping_)
    {
        setState(State::Stopped);
        return;
    }
    setState(State::Failed);
    emit errorOccurred(
        QStringLiteral("worker_exited"),
        QStringLiteral("Worker exited with code %1 (%2).")
            .arg(exitCode)
            .arg(exitStatus == QProcess::CrashExit ? QStringLiteral("crashed")
                                                   : QStringLiteral("normal")));
}

void WorkerClient::onProcessError(QProcess::ProcessError error)
{
    if (stopping_) return;
    fail(QStringLiteral("process_error"),
         QStringLiteral("Worker process error %1: %2")
             .arg(static_cast<int>(error))
             .arg(process_.errorString()));
}

QString WorkerClient::send(const QString& type, const QJsonObject& payload)
{
    const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    process_.write(
        protocol::encodeLine(protocol::makeMessage(requestId, type, payload)));
    return requestId;
}

void WorkerClient::handleMessage(const protocol::Message& message)
{
    if (message.type == QLatin1String(protocol::message_type::hello) &&
        message.requestId == helloRequestId_)
    {
        setState(State::Ready);
    }
    else if (message.type ==
                 QLatin1String(protocol::message_type::modelLoaded) &&
             message.requestId == loadRequestId_)
    {
        modelPath_ =
            message.payload.value(QStringLiteral("modelPath")).toString();
        setState(State::ModelReady);
        emit modelLoaded(
            modelPath_,
            message.payload.value(QStringLiteral("loadMilliseconds"))
                .toInteger(),
            message.payload.value(QStringLiteral("device")).toString());
    }
    else if (message.type ==
                 QLatin1String(protocol::message_type::modelUnloaded) &&
             message.requestId == unloadRequestId_)
    {
        modelPath_.clear();
        setState(State::Ready);
    }
    else if (message.type == QLatin1String(protocol::message_type::token) &&
             message.requestId == generationRequestId_)
    {
        emit tokenReceived(
            QByteArray::fromBase64(message.payload.value(QStringLiteral("data"))
                                       .toString()
                                       .toLatin1()));
    }
    else if (message.type ==
                 QLatin1String(protocol::message_type::generationFinished) &&
             message.requestId == generationRequestId_)
    {
        generationRequestId_.clear();
        setState(State::ModelReady);
        emit generationFinished(
            message.payload.value(QStringLiteral("cancelled")).toBool(),
            message.payload);
    }
    else if (message.type == QLatin1String(protocol::message_type::error))
    {
        const auto code =
            message.payload.value(QStringLiteral("code")).toString();
        const auto errorMessage =
            message.payload.value(QStringLiteral("message")).toString();
        if (message.requestId == loadRequestId_) setState(State::Ready);
        if (message.requestId == generationRequestId_)
        {
            generationRequestId_.clear();
            setState(State::ModelReady);
        }
        emit errorOccurred(code, errorMessage);
    }
}

void WorkerClient::setState(State state)
{
    if (state_ == state) return;
    state_ = state;
    emit stateChanged(state_);
}

void WorkerClient::fail(const QString& code, const QString& message)
{
    if (state_ == State::Starting) setState(State::Failed);
    emit errorOccurred(code, message);
}

QString WorkerClient::defaultWorkerPath() const
{
#ifdef Q_OS_WIN
    constexpr auto workerName = "qtllm-worker.exe";
#else
    constexpr auto workerName = "qtllm-worker";
#endif
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QString::fromLatin1(workerName));
}
}  // namespace qtllm::infrastructure
