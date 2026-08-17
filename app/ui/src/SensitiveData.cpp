#include "SensitiveData.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

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

QString redactSensitiveText(const QString& text, qsizetype maximumCharacters)
{
    auto redacted = text.trimmed();
    QJsonParseError parseError;
    const auto document =
        QJsonDocument::fromJson(redacted.toUtf8(), &parseError);
    if (parseError.error == QJsonParseError::NoError &&
        (document.isObject() || document.isArray()))
    {
        const auto value = document.isObject() ? QJsonValue(document.object())
                                               : QJsonValue(document.array());
        const auto safe = redactSensitiveValues(value);
        redacted = QString::fromUtf8(
            safe.isObject()
                ? QJsonDocument(safe.toObject()).toJson(QJsonDocument::Compact)
                : QJsonDocument(safe.toArray()).toJson(QJsonDocument::Compact));
    }

    static const QRegularExpression bearer(
        QStringLiteral(R"(\bBearer\s+[A-Za-z0-9._~+/=-]+)"),
        QRegularExpression::CaseInsensitiveOption);
    redacted.replace(bearer, QStringLiteral("Bearer [redacted]"));

    static const QRegularExpression assignment(
        QStringLiteral(
            R"(\b(password|passwd|token|secret|api[-_]?key|authorization|credential|cookie)\b(\s*[:=]\s*)("[^"]*"|'[^']*'|[^\s,;]+))"),
        QRegularExpression::CaseInsensitiveOption);
    redacted.replace(assignment, QStringLiteral("\\1\\2[redacted]"));

    const auto boundedMaximum = qMax<qsizetype>(64, maximumCharacters);
    if (redacted.size() > boundedMaximum)
        redacted = redacted.left(boundedMaximum) + QStringLiteral("...");
    return redacted;
}
}  // namespace qtllm::ui
