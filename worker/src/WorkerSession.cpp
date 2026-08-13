#include "WorkerSession.hpp"

#include "WorkerOptions.hpp"

#include "ProtocolVersion.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>

#include <cmath>
#include <limits>
#include <utility>

namespace qtllm::worker
{
namespace
{
bool readString(const QJsonObject& payload, const QString& name, QString& value,
                QString& errorMessage, bool required = false)
{
    const auto item = payload.value(name);
    if (item.isUndefined() && !required) return true;
    if (!item.isString() || (required && item.toString().isEmpty()))
    {
        errorMessage =
            QStringLiteral("payload.%1 must be a non-empty string.").arg(name);
        return false;
    }
    value = item.toString();
    return true;
}

template <typename Value>
bool readInteger(const QJsonObject& payload, const QString& name, Value minimum,
                 Value maximum, Value& value, QString& errorMessage)
{
    const auto item = payload.value(name);
    if (item.isUndefined()) return true;
    if (!item.isDouble())
    {
        errorMessage =
            QStringLiteral("payload.%1 must be an integer.").arg(name);
        return false;
    }
    const auto number = item.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum))
    {
        errorMessage = QStringLiteral("payload.%1 is out of range.").arg(name);
        return false;
    }
    value = static_cast<Value>(number);
    return true;
}

bool readFloat(const QJsonObject& payload, const QString& name, float minimum,
               float maximum, float& value, QString& errorMessage)
{
    const auto item = payload.value(name);
    if (item.isUndefined()) return true;
    if (!item.isDouble() || !std::isfinite(item.toDouble()) ||
        item.toDouble() < minimum || item.toDouble() > maximum)
    {
        errorMessage = QStringLiteral("payload.%1 is out of range.").arg(name);
        return false;
    }
    value = static_cast<float>(item.toDouble());
    return true;
}

bool readMessages(const QJsonObject& payload, WorkerOptions& options,
                  QString& errorMessage)
{
    const auto messagesValue = payload.value(QStringLiteral("messages"));
    if (messagesValue.isUndefined())
    {
        return readString(payload, QStringLiteral("prompt"), options.prompt,
                          errorMessage, true) &&
               readString(payload, QStringLiteral("systemPrompt"),
                          options.systemPrompt, errorMessage);
    }
    if (!messagesValue.isArray() || messagesValue.toArray().isEmpty())
    {
        errorMessage =
            QStringLiteral("payload.messages must be a non-empty array.");
        return false;
    }

    const auto messages = messagesValue.toArray();
    options.messages.reserve(messages.size());
    for (qsizetype index = 0; index < messages.size(); ++index)
    {
        const auto value = messages.at(index);
        if (!value.isObject())
        {
            errorMessage = QStringLiteral(
                               "payload.messages[%1] must be an "
                               "object.")
                               .arg(index);
            return false;
        }
        const auto object = value.toObject();
        const auto roleValue = object.value(QStringLiteral("role"));
        const auto contentValue = object.value(QStringLiteral("content"));
        chat::Role role;
        if (!roleValue.isString() ||
            !chat::parseRole(roleValue.toString(), role))
        {
            errorMessage =
                QStringLiteral("payload.messages[%1].role is invalid.")
                    .arg(index);
            return false;
        }
        if (!contentValue.isString() || contentValue.toString().isEmpty())
        {
            errorMessage = QStringLiteral(
                               "payload.messages[%1].content must "
                               "be a non-empty string.")
                               .arg(index);
            return false;
        }
        options.messages.append({role, contentValue.toString()});
    }

    qsizetype index = 0;
    if (options.messages.constFirst().role == chat::Role::System) ++index;
    auto expectedRole = chat::Role::User;
    for (; index < options.messages.size(); ++index)
    {
        if (options.messages.at(index).role != expectedRole)
        {
            errorMessage = QStringLiteral(
                "payload.messages must contain an optional leading system "
                "message followed by alternating user and assistant messages.");
            return false;
        }
        expectedRole = expectedRole == chat::Role::User ? chat::Role::Assistant
                                                        : chat::Role::User;
    }
    if (options.messages.constLast().role != chat::Role::User)
    {
        errorMessage =
            QStringLiteral("payload.messages must end with a user message.");
        return false;
    }
    return true;
}

bool parseGenerationOptions(const QJsonObject& payload, WorkerOptions& options,
                            QString& errorMessage)
{
    options.threads = qMax(1, QThread::idealThreadCount());
    return readMessages(payload, options, errorMessage) &&
           readInteger(payload, QStringLiteral("contextSize"), 256, 1'048'576,
                       options.contextSize, errorMessage) &&
           readInteger(payload, QStringLiteral("maxTokens"), 1, 1'048'576,
                       options.maxTokens, errorMessage) &&
           readInteger(payload, QStringLiteral("threads"), 1, 1024,
                       options.threads, errorMessage) &&
           readInteger(payload, QStringLiteral("topK"), 0, 1'000'000,
                       options.topK, errorMessage) &&
           readInteger(payload, QStringLiteral("seed"), std::uint32_t{0},
                       std::numeric_limits<std::uint32_t>::max(), options.seed,
                       errorMessage) &&
           readFloat(payload, QStringLiteral("temperature"), 0.0F, 10.0F,
                     options.temperature, errorMessage) &&
           readFloat(payload, QStringLiteral("topP"), 0.0F, 1.0F, options.topP,
                     errorMessage) &&
           readFloat(payload, QStringLiteral("repeatPenalty"), 0.0F, 10.0F,
                     options.repeatPenalty, errorMessage);
}
}  // namespace

WorkerSession::WorkerSession(QObject* parent) : QObject(parent)
{
}

WorkerSession::~WorkerSession()
{
    cancelRequested_.store(true, std::memory_order_relaxed);
    engine_.cancel();
    if (generationThread_.joinable()) generationThread_.join();
}

bool WorkerSession::start(QString& errorMessage)
{
    if (!output_.open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered))
    {
        errorMessage = QStringLiteral("Unable to open stdout.");
        return false;
    }
    return true;
}

void WorkerSession::processLine(const QByteArray& line)
{
    protocol::Message message;
    QString errorMessage;
    if (!protocol::decodeLine(line, message, errorMessage))
    {
        sendError(QStringLiteral("invalid-message"),
                  QStringLiteral("invalid_message"), errorMessage);
        return;
    }
    if (message.protocolVersion != protocol::version)
    {
        sendError(message.requestId, QStringLiteral("protocol_mismatch"),
                  QStringLiteral("Unsupported protocol version %1; expected "
                                 "%2.")
                      .arg(message.protocolVersion)
                      .arg(protocol::version));
        return;
    }
    dispatch(message);
}

void WorkerSession::dispatch(const protocol::Message& message)
{
    if (message.type == QLatin1String(protocol::message_type::hello))
        handleHello(message);
    else if (message.type == QLatin1String(protocol::message_type::loadModel))
        handleLoadModel(message);
    else if (message.type == QLatin1String(protocol::message_type::unloadModel))
        handleUnloadModel(message);
    else if (message.type == QLatin1String(protocol::message_type::generate))
        handleGenerate(message);
    else if (message.type == QLatin1String(protocol::message_type::cancel))
        handleCancel(message);
    else if (message.type == QLatin1String(protocol::message_type::getStatus))
        handleGetStatus(message);
    else
        sendError(message.requestId, QStringLiteral("unknown_type"),
                  QStringLiteral("Unknown message type: %1").arg(message.type));
}

void WorkerSession::handleHello(const protocol::Message& message)
{
    send(protocol::makeMessage(
        message.requestId, QString::fromLatin1(protocol::message_type::hello),
        {{QStringLiteral("workerVersion"), QStringLiteral("0.1.0")},
         {QStringLiteral("protocolVersion"), protocol::version}}));
}

void WorkerSession::handleLoadModel(const protocol::Message& message)
{
    if (generating_)
    {
        sendError(message.requestId, QStringLiteral("busy"),
                  QStringLiteral("Cannot load a model while generating."));
        return;
    }

    QString modelPath;
    QString errorMessage;
    int gpuLayers = 0;
    if (!readString(message.payload, QStringLiteral("modelPath"), modelPath,
                    errorMessage, true) ||
        !readInteger(message.payload, QStringLiteral("gpuLayers"), 0, 10'000,
                     gpuLayers, errorMessage))
    {
        sendError(message.requestId, QStringLiteral("invalid_payload"),
                  errorMessage);
        return;
    }
    const QFileInfo modelInfo(modelPath);
    if (!modelInfo.exists() || !modelInfo.isFile())
    {
        sendError(
            message.requestId, QStringLiteral("model_not_found"),
            QStringLiteral("Model file does not exist: %1").arg(modelPath));
        return;
    }

    qint64 loadMilliseconds = 0;
    if (!engine_.loadModel(modelInfo.absoluteFilePath(), gpuLayers,
                           errorMessage, &loadMilliseconds))
    {
        sendError(message.requestId, QStringLiteral("model_load_failed"),
                  errorMessage);
        return;
    }
    send(protocol::makeMessage(
        message.requestId,
        QString::fromLatin1(protocol::message_type::modelLoaded),
        {{QStringLiteral("modelPath"), engine_.modelPath()},
         {QStringLiteral("loadMilliseconds"), loadMilliseconds}}));
}

void WorkerSession::handleUnloadModel(const protocol::Message& message)
{
    if (generating_)
    {
        sendError(message.requestId, QStringLiteral("busy"),
                  QStringLiteral("Cannot unload the model while generating."));
        return;
    }
    engine_.unloadModel();
    send(protocol::makeMessage(
        message.requestId,
        QString::fromLatin1(protocol::message_type::modelUnloaded)));
}

void WorkerSession::handleGenerate(const protocol::Message& message)
{
    if (generating_)
    {
        sendError(message.requestId, QStringLiteral("busy"),
                  QStringLiteral("A generation is already active."));
        return;
    }
    WorkerOptions options;
    QString errorMessage;
    if (!parseGenerationOptions(message.payload, options, errorMessage))
    {
        sendError(message.requestId, QStringLiteral("invalid_payload"),
                  errorMessage);
        return;
    }
    if (!engine_.isModelLoaded())
    {
        sendError(message.requestId, QStringLiteral("model_not_loaded"),
                  QStringLiteral("Load a model before generating."));
        return;
    }

    if (generationThread_.joinable()) generationThread_.join();
    generating_ = true;
    generationRequestId_ = message.requestId;
    cancelRequested_.store(false, std::memory_order_relaxed);
    send(protocol::makeMessage(
        message.requestId,
        QString::fromLatin1(protocol::message_type::generationStarted)));

    generationThread_ = std::thread(
        [this, requestId = message.requestId, options = std::move(options)]
        {
            QString generationError;
            GenerationMetrics metrics;
            const auto generated = engine_.generate(
                options,
                [this, requestId](const QByteArray& piece)
                {
                    QMetaObject::invokeMethod(
                        this,
                        [this, requestId, piece]
                        {
                            send(protocol::makeMessage(
                                requestId,
                                QString::fromLatin1(
                                    protocol::message_type::token),
                                {{QStringLiteral("data"),
                                  QString::fromLatin1(piece.toBase64())},
                                 {QStringLiteral("encoding"),
                                  QStringLiteral("base64")}}));
                        });
                },
                generationError, &metrics, &cancelRequested_);
            QMetaObject::invokeMethod(
                this,
                [this, requestId, generated, generationError, metrics]
                {
                    finishGeneration(requestId, generated, generationError,
                                     metrics);
                });
        });
}

void WorkerSession::handleCancel(const protocol::Message& message)
{
    const auto targetRequestId =
        message.payload.value(QStringLiteral("targetRequestId")).toString();
    const auto accepted =
        generating_ &&
        (targetRequestId.isEmpty() || targetRequestId == generationRequestId_);
    if (accepted)
    {
        cancelRequested_.store(true, std::memory_order_relaxed);
        engine_.cancel();
    }
    sendStatus(message.requestId,
               {{QStringLiteral("cancelAccepted"), accepted}});
}

void WorkerSession::handleGetStatus(const protocol::Message& message)
{
    sendStatus(message.requestId);
}

void WorkerSession::finishGeneration(const QString& requestId, bool generated,
                                     const QString& errorMessage,
                                     const GenerationMetrics& metrics)
{
    const auto cancelled =
        cancelRequested_.load(std::memory_order_relaxed) ||
        errorMessage == QStringLiteral("Generation cancelled.");
    generating_ = false;
    generationRequestId_.clear();

    if (!generated && !cancelled)
    {
        sendError(requestId, QStringLiteral("generation_failed"), errorMessage);
        return;
    }
    send(protocol::makeMessage(
        requestId,
        QString::fromLatin1(protocol::message_type::generationFinished),
        {{QStringLiteral("cancelled"), cancelled},
         {QStringLiteral("promptEvaluationMilliseconds"),
          metrics.promptEvaluationMilliseconds},
         {QStringLiteral("firstTokenMilliseconds"),
          metrics.firstTokenMilliseconds},
         {QStringLiteral("promptTokens"), metrics.promptTokens},
         {QStringLiteral("generatedTokens"), metrics.generatedTokens},
         {QStringLiteral("discardedMessages"), metrics.discardedMessages},
         {QStringLiteral("tokensPerSecond"),
          metrics.generationTokensPerSecond}}));
}

void WorkerSession::send(const protocol::Message& message)
{
    const QMutexLocker locker(&outputMutex_);
    output_.write(protocol::encodeLine(message));
    output_.flush();
}

void WorkerSession::sendError(const QString& requestId, const QString& code,
                              const QString& message)
{
    send(protocol::makeMessage(
        requestId, QString::fromLatin1(protocol::message_type::error),
        {{QStringLiteral("code"), code},
         {QStringLiteral("message"), message}}));
}

void WorkerSession::sendStatus(const QString& requestId,
                               const QJsonObject& additionalPayload)
{
    QJsonObject payload{
        {QStringLiteral("modelLoaded"), engine_.isModelLoaded()},
        {QStringLiteral("modelPath"), engine_.modelPath()},
        {QStringLiteral("generating"), generating_},
        {QStringLiteral("generationRequestId"), generationRequestId_}};
    for (auto item = additionalPayload.constBegin();
         item != additionalPayload.constEnd(); ++item)
        payload.insert(item.key(), item.value());
    send(protocol::makeMessage(
        requestId, QString::fromLatin1(protocol::message_type::status),
        payload));
}
}  // namespace qtllm::worker
