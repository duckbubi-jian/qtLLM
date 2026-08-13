#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace qtllm::protocol
{
struct Message
{
    int protocolVersion = 0;
    QString requestId;
    QString type;
    QJsonObject payload;
};

namespace message_type
{
inline constexpr auto hello = "hello";
inline constexpr auto loadModel = "load_model";
inline constexpr auto modelLoaded = "model_loaded";
inline constexpr auto unloadModel = "unload_model";
inline constexpr auto modelUnloaded = "model_unloaded";
inline constexpr auto generate = "generate";
inline constexpr auto generationStarted = "generation_started";
inline constexpr auto cancel = "cancel";
inline constexpr auto getStatus = "get_status";
inline constexpr auto status = "status";
inline constexpr auto token = "token";
inline constexpr auto generationFinished = "generation_finished";
inline constexpr auto error = "error";
}  // namespace message_type

QByteArray encodeLine(const Message& message);
bool decodeLine(const QByteArray& line, Message& message,
                QString& errorMessage);
Message makeMessage(const QString& requestId, const QString& type,
                    const QJsonObject& payload = {});
}  // namespace qtllm::protocol
