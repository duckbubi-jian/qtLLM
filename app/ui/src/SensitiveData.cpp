#include "SensitiveData.hpp"

#include <QJsonArray>
#include <QJsonObject>

namespace qtllm::ui
{
namespace
{
bool isSensitiveKey(const QString& key)
{
    auto normalized = key.toLower();
    normalized.remove(QLatin1Char('_'));
    normalized.remove(QLatin1Char('-'));
    return normalized.contains(QStringLiteral("password")) ||
           normalized.contains(QStringLiteral("passwd")) ||
           normalized.contains(QStringLiteral("token")) ||
           normalized.contains(QStringLiteral("secret")) ||
           normalized.contains(QStringLiteral("apikey")) ||
           normalized.contains(QStringLiteral("authorization")) ||
           normalized.contains(QStringLiteral("credential")) ||
           normalized.contains(QStringLiteral("cookie"));
}
}  // namespace

QJsonValue redactSensitiveValues(const QJsonValue& value)
{
    if (value.isArray())
    {
        QJsonArray redacted;
        for (const auto& item : value.toArray())
            redacted.append(redactSensitiveValues(item));
        return redacted;
    }
    if (!value.isObject()) return value;

    QJsonObject redacted;
    const auto object = value.toObject();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        redacted.insert(item.key(),
                        isSensitiveKey(item.key())
                            ? QJsonValue(QStringLiteral("[redacted]"))
                            : redactSensitiveValues(item.value()));
    }
    return redacted;
}
}  // namespace qtllm::ui
