#include "ToolEffectVerifier.hpp"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

namespace qtllm::application
{
namespace
{
constexpr qsizetype maximumTraversalDepth = 12;
constexpr qsizetype maximumVisitedValues = 8'192;
constexpr qsizetype maximumExpectations = 128;

QString normalizedKey(QString key)
{
    key = key.toLower();
    key.remove(QLatin1Char('_'));
    key.remove(QLatin1Char('-'));
    key.remove(QLatin1Char(' '));
    return key;
}

bool isScalar(const QJsonValue& value)
{
    return value.isString() || value.isDouble() || value.isBool() ||
           value.isNull();
}

bool isScalarArray(const QJsonValue& value)
{
    if (!value.isArray()) return false;
    const auto array = value.toArray();
    return std::all_of(array.cbegin(), array.cend(),
                       [](const QJsonValue& item) { return isScalar(item); });
}

QByteArray scalarDigest(const QJsonValue& value)
{
    const auto serialized =
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QCryptographicHash::hash(serialized, QCryptographicHash::Sha256);
}

bool isSensitiveKey(const QString& normalized)
{
    return normalized.contains(QStringLiteral("password")) ||
           normalized.contains(QStringLiteral("passwd")) ||
           normalized.contains(QStringLiteral("secret")) ||
           normalized.contains(QStringLiteral("token")) ||
           normalized.contains(QStringLiteral("apikey")) ||
           normalized.contains(QStringLiteral("credential")) ||
           normalized.contains(QStringLiteral("privatekey"));
}

bool isIdentityKey(const QString& key)
{
    const auto normalized = normalizedKey(key);
    if (normalized == QLatin1String("id") ||
        normalized == QLatin1String("uuid"))
        return true;
    if (normalized.endsWith(QStringLiteral("uuid"))) return true;
    if (key.endsWith(QStringLiteral("_id"), Qt::CaseInsensitive) ||
        key.endsWith(QStringLiteral("-id"), Qt::CaseInsensitive) ||
        key.endsWith(QStringLiteral(" id"), Qt::CaseInsensitive) ||
        key.endsWith(QStringLiteral("Id")) ||
        key.endsWith(QStringLiteral("ID")))
        return true;
    return normalized == QLatin1String("objectid") ||
           normalized == QLatin1String("resourceid") ||
           normalized == QLatin1String("targetid") ||
           normalized == QLatin1String("itemid") ||
           normalized == QLatin1String("entityid") ||
           normalized == QLatin1String("recordid") ||
           normalized == QLatin1String("modelid") ||
           normalized == QLatin1String("jobid") ||
           normalized == QLatin1String("taskid") ||
           normalized == QLatin1String("requestid") ||
           normalized == QLatin1String("runid");
}

bool isParentKey(const QString& normalized)
{
    return normalized == QLatin1String("parent") ||
           normalized.startsWith(QStringLiteral("parent")) ||
           normalized.endsWith(QStringLiteral("parent"));
}

bool isNameKey(const QString& normalized)
{
    return normalized == QLatin1String("name") ||
           normalized == QLatin1String("displayname") ||
           normalized == QLatin1String("label") ||
           normalized.endsWith(QStringLiteral("name"));
}

bool isLocatorKey(const QString& normalized)
{
    return normalized.endsWith(QStringLiteral("path")) ||
           normalized.endsWith(QStringLiteral("uri")) ||
           normalized.endsWith(QStringLiteral("url")) ||
           normalized.endsWith(QStringLiteral("file")) ||
           normalized.endsWith(QStringLiteral("filename")) ||
           normalized.startsWith(QStringLiteral("source"));
}

bool isControlKey(const QString& normalized)
{
    return normalized == QLatin1String("query") ||
           normalized == QLatin1String("filter") ||
           normalized == QLatin1String("cursor") ||
           normalized == QLatin1String("offset") ||
           normalized == QLatin1String("limit") ||
           normalized == QLatin1String("page") ||
           normalized == QLatin1String("pagesize") ||
           normalized == QLatin1String("timeout") ||
           normalized == QLatin1String("timeoutms") ||
           normalized == QLatin1String("wait") ||
           normalized == QLatin1String("dryrun") ||
           normalized == QLatin1String("ifmatch") ||
           normalized == QLatin1String("expectedrevision") ||
           normalized == QLatin1String("oldvalue");
}

bool containsStableIdentity(const QJsonValue& value, qsizetype depth,
                            qsizetype& visited, bool parentContext)
{
    if (depth > maximumTraversalDepth || visited >= maximumVisitedValues)
        return false;
    ++visited;
    if (value.isArray())
    {
        for (const auto& item : value.toArray())
            if (containsStableIdentity(item, depth + 1, visited, parentContext))
                return true;
        return false;
    }
    if (!value.isObject()) return false;

    const auto object = value.toObject();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        const auto normalized = normalizedKey(item.key());
        const auto nestedParent = parentContext || isParentKey(normalized);
        if (!nestedParent && isIdentityKey(item.key()) &&
            isScalar(item.value()) && !item.value().isNull())
            return true;
        if ((item.value().isObject() || item.value().isArray()) &&
            containsStableIdentity(item.value(), depth + 1, visited,
                                   nestedParent))
            return true;
    }
    return false;
}

bool isEffectField(const QString& key, const QString& normalized,
                   bool hasStableIdentity, bool parentContext)
{
    if (parentContext || isParentKey(normalized) || isIdentityKey(key) ||
        isSensitiveKey(normalized) || isLocatorKey(normalized) ||
        isControlKey(normalized))
        return false;
    if (isNameKey(normalized) && !hasStableIdentity) return false;
    return true;
}

void collectExpectations(const QJsonValue& value, const QString& path,
                         bool hasStableIdentity, bool parentContext,
                         qsizetype depth, qsizetype& visited,
                         QList<ToolEffectExpectation>& expectations)
{
    if (depth > maximumTraversalDepth || visited >= maximumVisitedValues ||
        expectations.size() >= maximumExpectations)
        return;
    ++visited;
    if (value.isArray())
    {
        const auto array = value.toArray();
        for (qsizetype index = 0;
             index < array.size() && expectations.size() < maximumExpectations;
             ++index)
            collectExpectations(array.at(index),
                                path + QStringLiteral("[%1]").arg(index),
                                hasStableIdentity, parentContext, depth + 1,
                                visited, expectations);
        return;
    }
    if (!value.isObject()) return;

    const auto object = value.toObject();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        const auto normalized = normalizedKey(item.key());
        const auto fieldPath =
            path.isEmpty() ? item.key() : path + QLatin1Char('.') + item.key();
        const auto nestedParent = parentContext || isParentKey(normalized);
        if (isScalar(item.value()) || isScalarArray(item.value()))
        {
            if (isEffectField(item.key(), normalized, hasStableIdentity,
                              nestedParent))
                expectations.append(
                    {fieldPath, normalized, scalarDigest(item.value())});
            continue;
        }
        collectExpectations(item.value(), fieldPath, hasStableIdentity,
                            nestedParent, depth + 1, visited, expectations);
    }
}

void collectReadBackFields(const QJsonValue& value, qsizetype depth,
                           qsizetype& visited,
                           QHash<QString, QList<QByteArray>>& fields)
{
    if (depth > maximumTraversalDepth || visited >= maximumVisitedValues)
        return;
    ++visited;
    if (value.isArray())
    {
        for (const auto& item : value.toArray())
            collectReadBackFields(item, depth + 1, visited, fields);
        return;
    }
    if (!value.isObject()) return;

    const auto object = value.toObject();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        if (isScalar(item.value()) || isScalarArray(item.value()))
        {
            fields[normalizedKey(item.key())].append(
                scalarDigest(item.value()));
            continue;
        }
        collectReadBackFields(item.value(), depth + 1, visited, fields);
    }
}

void makeUnique(QStringList& fields)
{
    std::sort(fields.begin(), fields.end());
    fields.erase(std::unique(fields.begin(), fields.end()), fields.end());
}
}  // namespace

QList<ToolEffectExpectation> ToolEffectVerifier::captureExpectations(
    const QJsonObject& arguments)
{
    auto visited = qsizetype{0};
    const auto hasStableIdentity =
        containsStableIdentity(arguments, 0, visited, false);
    QList<ToolEffectExpectation> expectations;
    visited = 0;
    collectExpectations(arguments, {}, hasStableIdentity, false, 0, visited,
                        expectations);
    return expectations;
}

ToolEffectVerificationResult ToolEffectVerifier::verify(
    const QList<ToolEffectExpectation>& expectations,
    const QJsonValue& readBack)
{
    ToolEffectVerificationResult result;
    if (expectations.isEmpty())
    {
        result.status = ToolEffectVerificationStatus::Verified;
        return result;
    }

    QHash<QString, QList<QByteArray>> fields;
    auto visited = qsizetype{0};
    collectReadBackFields(readBack, 0, visited, fields);
    for (const auto& expectation : expectations)
    {
        auto candidates = fields.find(expectation.normalizedField);
        if (candidates == fields.end())
        {
            result.missingFields.append(expectation.field);
            continue;
        }
        const auto matchingDigest = std::find(
            candidates->begin(), candidates->end(), expectation.expectedDigest);
        if (matchingDigest != candidates->end())
        {
            result.matchedFields.append(expectation.field);
            candidates->erase(matchingDigest);
        }
        else
            result.mismatchedFields.append(expectation.field);
    }
    makeUnique(result.matchedFields);
    makeUnique(result.missingFields);
    makeUnique(result.mismatchedFields);
    if (!result.mismatchedFields.isEmpty())
        result.status = ToolEffectVerificationStatus::Mismatch;
    else if (!result.missingFields.isEmpty())
        result.status = ToolEffectVerificationStatus::Incomplete;
    else
        result.status = ToolEffectVerificationStatus::Verified;
    return result;
}
}  // namespace qtllm::application
