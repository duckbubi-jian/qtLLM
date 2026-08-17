#include "WorkerClient.hpp"

#include "ComputeProtocol.hpp"
#include "Logging.hpp"
#include "ProtocolVersion.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QThread>
#include <QUuid>

#include <utility>

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
    auto environment = QProcessEnvironment::systemEnvironment();
    if (!logging::logDirectory().isEmpty())
        environment.insert(QStringLiteral("QTLLM_LOG_DIR"),
                           logging::logDirectory());
    process_.setProcessEnvironment(environment);
    process_.setProgram(executable);
    process_.setArguments({QStringLiteral("--ipc")});
    logging::info(QStringLiteral("Starting worker: %1 --ipc").arg(executable));
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

void WorkerClient::loadModel(const QString& modelPath,
                             const inference::ModelLoadOptions& options)
{
    if (state_ != State::Ready && state_ != State::ModelReady)
    {
        fail(QStringLiteral("invalid_state"),
             QStringLiteral("Worker is not ready to load a model."));
        return;
    }
    QString errorMessage;
    if (!inference::validateModelLoadOptions(options, errorMessage))
    {
        fail(QStringLiteral("invalid_model_load_options"), errorMessage);
        return;
    }
    modelPath_.clear();
    activeComputeDevices_.clear();
    activeSplitMode_ = QStringLiteral("cpu");
    setState(State::LoadingModel);
    logging::info(
        QStringLiteral("Loading model: %1; gpuLayers=%2; "
                       "placement=%3")
            .arg(modelPath)
            .arg(options.gpuLayers)
            .arg(inference::devicePlacementModeName(options.placementMode)));
    auto payload = protocol::serializeModelLoadOptions(options);
    payload.insert(QStringLiteral("modelPath"), modelPath);
    loadRequestId_ =
        send(QString::fromLatin1(protocol::message_type::loadModel), payload);
}

void WorkerClient::unloadModel()
{
    if (state_ != State::ModelReady) return;
    setState(State::UnloadingModel);
    unloadRequestId_ =
        send(QString::fromLatin1(protocol::message_type::unloadModel));
}

void WorkerClient::refreshComputeDevices()
{
    if (!capabilities_.gpuDeviceDiscovery ||
        process_.state() != QProcess::Running)
        return;
    devicesRequestId_ =
        send(QString::fromLatin1(protocol::message_type::listDevices));
}

void WorkerClient::generate(const QString& prompt, const QString& systemPrompt,
                            int contextSize, int maxTokens, int threads,
                            float temperature, float topP, int topK,
                            float repeatPenalty,
                            inference::ResponseMode responseMode,
                            const QString& grammar)
{
    QList<chat::Message> messages;
    if (!systemPrompt.isEmpty())
        messages.append({chat::Role::System, systemPrompt});
    messages.append({chat::Role::User, prompt});
    generate(messages, contextSize, maxTokens, threads, temperature, topP, topK,
             repeatPenalty, responseMode, grammar);
}

void WorkerClient::generate(const QList<chat::Message>& messages,
                            int contextSize, int maxTokens, int threads,
                            float temperature, float topP, int topK,
                            float repeatPenalty,
                            inference::ResponseMode responseMode,
                            const QString& grammar)
{
    if (state_ != State::ModelReady)
    {
        fail(QStringLiteral("invalid_state"),
             QStringLiteral("Load a model before generating."));
        return;
    }
    if (threads <= 0) threads = qMax(1, QThread::idealThreadCount());

    logging::info(
        QStringLiteral("Starting generation: mode=%1 messages=%2 context=%3 "
                       "maxTokens=%4 threads=%5")
            .arg(inference::responseModeName(responseMode))
            .arg(messages.size())
            .arg(contextSize)
            .arg(maxTokens)
            .arg(threads));

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
              {QStringLiteral("repeatPenalty"), repeatPenalty},
              {QStringLiteral("responseMode"),
               inference::responseModeName(responseMode)},
              {QStringLiteral("grammar"), grammar}});
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

WorkerClient::Capabilities WorkerClient::capabilities() const
{
    return capabilities_;
}

QList<inference::ComputeDevice> WorkerClient::computeDevices() const
{
    return computeDevices_;
}

QList<inference::ComputeDevice> WorkerClient::activeComputeDevices() const
{
    return activeComputeDevices_;
}

QString WorkerClient::activeSplitMode() const
{
    return activeSplitMode_;
}

void WorkerClient::onStarted()
{
    logging::info(QStringLiteral("Worker process started; pid=%1")
                      .arg(process_.processId()));
    helloRequestId_ =
        send(QString::fromLatin1(protocol::message_type::hello),
             {{QStringLiteral("clientVersion"), QStringLiteral("0.2.0")}});
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
    if (!text.isEmpty())
    {
        logging::warning(
            QStringLiteral("Worker stderr: %1").arg(text.trimmed()));
        emit diagnosticReceived(text);
    }
}

void WorkerClient::onFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    helloRequestId_.clear();
    loadRequestId_.clear();
    unloadRequestId_.clear();
    devicesRequestId_.clear();
    modelPath_.clear();
    generationRequestId_.clear();
    capabilities_ = {};
    computeDevices_.clear();
    activeComputeDevices_.clear();
    activeSplitMode_ = QStringLiteral("cpu");
    if (stopping_)
    {
        logging::info(
            QStringLiteral("Worker stopped with exit code %1").arg(exitCode));
        setState(State::Stopped);
        return;
    }
    logging::critical(
        QStringLiteral("Worker exited unexpectedly: code=%1 status=%2")
            .arg(exitCode)
            .arg(exitStatus == QProcess::CrashExit ? QStringLiteral("crashed")
                                                   : QStringLiteral("normal")));
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
    logging::error(QStringLiteral("Worker process error %1: %2")
                       .arg(static_cast<int>(error))
                       .arg(process_.errorString()));
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
        const auto capabilities =
            message.payload.value(QStringLiteral("capabilities")).toObject();
        capabilities_.structuredGeneration =
            capabilities.value(QStringLiteral("structuredGeneration")).toBool();
        capabilities_.grammar =
            capabilities.value(QStringLiteral("grammar")).toBool();
        capabilities_.gpuDeviceDiscovery =
            capabilities.value(QStringLiteral("gpuDeviceDiscovery")).toBool();
        capabilities_.multiGpuLayerSplit =
            capabilities.value(QStringLiteral("multiGpuLayerSplit")).toBool();
        setState(State::Ready);
        refreshComputeDevices();
    }
    else if (message.type == QLatin1String(protocol::message_type::devices) &&
             message.requestId == devicesRequestId_)
    {
        QList<inference::ComputeDevice> devices;
        QString errorMessage;
        if (!message.payload.value(QStringLiteral("devices")).isArray() ||
            !protocol::parseComputeDevices(
                message.payload.value(QStringLiteral("devices")).toArray(),
                devices, errorMessage))
        {
            fail(QStringLiteral("invalid_device_list"), errorMessage);
            return;
        }
        devicesRequestId_.clear();
        computeDevices_ = std::move(devices);
        emit computeDevicesChanged(computeDevices_);
    }
    else if (message.type ==
                 QLatin1String(protocol::message_type::modelLoaded) &&
             message.requestId == loadRequestId_)
    {
        modelPath_ =
            message.payload.value(QStringLiteral("modelPath")).toString();
        activeSplitMode_ =
            message.payload.value(QStringLiteral("splitMode")).toString();
        QString deviceError;
        if (!message.payload.value(QStringLiteral("devices")).isArray() ||
            !protocol::parseComputeDevices(
                message.payload.value(QStringLiteral("devices")).toArray(),
                activeComputeDevices_, deviceError))
        {
            fail(QStringLiteral("invalid_active_device_list"), deviceError);
            return;
        }
        loadRequestId_.clear();
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
        activeComputeDevices_.clear();
        activeSplitMode_ = QStringLiteral("cpu");
        unloadRequestId_.clear();
        setState(State::Ready);
        emit modelUnloaded();
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
        if (message.requestId == loadRequestId_)
        {
            loadRequestId_.clear();
            modelPath_.clear();
            setState(State::Ready);
            emit modelLoadFailed(code, errorMessage);
        }
        else if (message.requestId == devicesRequestId_)
        {
            devicesRequestId_.clear();
        }
        else if (message.requestId == unloadRequestId_)
        {
            unloadRequestId_.clear();
            setState(State::ModelReady);
            emit modelUnloadFailed(code, errorMessage);
        }
        else if (message.requestId == generationRequestId_)
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
