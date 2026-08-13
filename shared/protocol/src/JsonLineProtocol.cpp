#include "JsonLineProtocol.hpp"

#include "ProtocolVersion.hpp"

#include <QJsonDocument>
#include <QJsonParseError>

#include <cmath>
#include <limits>

namespace qtllm::protocol
{
QByteArray encodeLine(const Message& message)
{
    QJsonObject object{
        {QStringLiteral("protocolVersion"), message.protocolVersion},
        {QStringLiteral("requestId"), message.requestId},
        {QStringLiteral("type"), message.type},
        {QStringLiteral("payload"), message.payload}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

bool decodeLine(const QByteArray& line, Message& message, QString& errorMessage)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = QStringLiteral("Invalid JSON object: %1")
                           .arg(parseError.errorString());
        return false;
    }

    const auto object = document.object();
    const auto protocolValue = object.value(QStringLiteral("protocolVersion"));
    const auto requestIdValue = object.value(QStringLiteral("requestId"));
    const auto typeValue = object.value(QStringLiteral("type"));
    const auto payloadValue = object.value(QStringLiteral("payload"));
    if (!protocolValue.isDouble() || !requestIdValue.isString() ||
        requestIdValue.toString().isEmpty() || !typeValue.isString() ||
        typeValue.toString().isEmpty() || !payloadValue.isObject())
    {
        errorMessage = QStringLiteral(
            "Message requires integer protocolVersion, non-empty requestId "
            "and type, and object payload.");
        return false;
    }

    const auto protocolNumber = protocolValue.toDouble();
    if (!std::isfinite(protocolNumber) ||
        protocolNumber < std::numeric_limits<int>::min() ||
        protocolNumber > std::numeric_limits<int>::max())
    {
        errorMessage = QStringLiteral("protocolVersion is out of range.");
        return false;
    }
    const auto protocolInteger = static_cast<int>(protocolNumber);
    if (protocolNumber != protocolInteger)
    {
        errorMessage = QStringLiteral("protocolVersion must be an integer.");
        return false;
    }

    message.protocolVersion = protocolInteger;
    message.requestId = requestIdValue.toString();
    message.type = typeValue.toString();
    message.payload = payloadValue.toObject();
    return true;
}

Message makeMessage(const QString& requestId, const QString& type,
                    const QJsonObject& payload)
{
    return {version, requestId, type, payload};
}
}  // namespace qtllm::protocol
