#include "AgentLedger.hpp"
#include "ToolResultStatus.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <optional>
#include <utility>

namespace qtllm::application
{
namespace
{
constexpr qsizetype maximumResources = 64;
constexpr qsizetype maximumJobs = 32;
constexpr qsizetype maximumVerifications = 64;
constexpr qsizetype maximumTraversalDepth = 12;
constexpr qsizetype maximumVisitedValues = 8'192;
constexpr qsizetype maximumFieldCharacters = 512;
constexpr qsizetype maximumJobArgumentBytes = 2'048;
constexpr qsizetype maximumSemanticTokens = 64;

QString bounded(QString value)
{
    value = value.trimmed();
    return value.size() <= maximumFieldCharacters
               ? value
               : value.left(maximumFieldCharacters);
}

QString normalizedKey(QString key)
{
    key = key.toLower();
    key.remove(QLatin1Char('_'));
    key.remove(QLatin1Char('-'));
    key.remove(QLatin1Char(' '));
    return key;
}

QString scalarText(const QJsonValue& value)
{
    if (value.isString()) return bounded(value.toString());
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 16);
    return {};
}

QString comparableToken(QString value)
{
    value = bounded(std::move(value));
    value.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return value.toCaseFolded();
}

QJsonObject boundedArguments(const QJsonObject& arguments)
{
    const auto serialized =
        QJsonDocument(arguments).toJson(QJsonDocument::Compact);
    if (serialized.size() <= maximumJobArgumentBytes) return arguments;
    return {{QStringLiteral("omitted"), true}};
}

bool isParentIdentityKey(const QString& key)
{
    const auto normalized = normalizedKey(key);
    return normalized.contains(QStringLiteral("parent")) &&
           (normalized.endsWith(QStringLiteral("uuid")) ||
            normalized.endsWith(QStringLiteral("id")));
}

bool isParentContextKey(const QString& key)
{
    const auto normalized = normalizedKey(key);
    return normalized == QLatin1String("parent") ||
           normalized.startsWith(QStringLiteral("parent")) ||
           normalized.endsWith(QStringLiteral("parent"));
}

bool isJobIdentityKey(const QString& key)
{
    const auto normalized = normalizedKey(key);
    if (!normalized.endsWith(QStringLiteral("uuid")) &&
        !normalized.endsWith(QStringLiteral("id")))
        return false;
    return normalized.contains(QStringLiteral("job")) ||
           normalized.contains(QStringLiteral("task")) ||
           normalized.contains(QStringLiteral("operation")) ||
           normalized.contains(QStringLiteral("process")) ||
           normalized.contains(QStringLiteral("request")) ||
           normalized == QLatin1String("runid");
}

int identityKeyScore(const QString& key)
{
    if (isParentIdentityKey(key) || isJobIdentityKey(key)) return -1;
    const auto normalized = normalizedKey(key);
    if (normalized == QLatin1String("objectuuid")) return 120;
    if (normalized == QLatin1String("resourceuuid")) return 110;
    if (normalized == QLatin1String("uuid")) return 100;
    if (normalized.endsWith(QStringLiteral("uuid"))) return 90;
    if (normalized == QLatin1String("id")) return 80;
    if (normalized.endsWith(QStringLiteral("id"))) return 70;
    return -1;
}

int parentKeyScore(const QString& key)
{
    if (!isParentIdentityKey(key)) return -1;
    const auto normalized = normalizedKey(key);
    if (normalized == QLatin1String("parentuuid")) return 100;
    if (normalized.endsWith(QStringLiteral("uuid"))) return 90;
    if (normalized == QLatin1String("parentid")) return 80;
    return 70;
}

int nameKeyScore(const QString& key)
{
    const auto normalized = normalizedKey(key);
    if (normalized == QLatin1String("name")) return 100;
    if (normalized == QLatin1String("displayname")) return 90;
    if (normalized == QLatin1String("label")) return 80;
    if (normalized.endsWith(QStringLiteral("name"))) return 60;
    return -1;
}

int kindKeyScore(const QString& key)
{
    const auto normalized = normalizedKey(key);
    if (normalized == QLatin1String("itemtype")) return 100;
    if (normalized == QLatin1String("resourcetype") ||
        normalized == QLatin1String("objecttype") ||
        normalized == QLatin1String("modeltype"))
        return 90;
    if (normalized == QLatin1String("kind")) return 80;
    if (normalized == QLatin1String("type")) return 70;
    return -1;
}

QJsonValue resultPayload(const agent::ToolResult& result)
{
    if (!result.structuredContent.isUndefined() &&
        !result.structuredContent.isNull())
        return result.structuredContent;
    const auto structured =
        result.result.value(QStringLiteral("structuredContent"));
    if (!structured.isUndefined() && !structured.isNull()) return structured;
    return result.result;
}

void collectSemanticTokens(const QJsonValue& value, QStringList& identities,
                           QStringList& names, qsizetype depth,
                           qsizetype& visited, bool includeAliases,
                           bool parentContext);
bool intersects(const QStringList& left, const QStringList& right);

bool verificationResolved(const QString& state)
{
    return state == QLatin1String("verified") ||
           state == QLatin1String("accepted") ||
           state == QLatin1String("attested");
}

bool explicitlyConfirmsSuccess(const QJsonValue& value)
{
    if (!value.isObject()) return false;

    const auto object = value.toObject();
    for (auto entry = object.constBegin(); entry != object.constEnd(); ++entry)
    {
        const auto key = normalizedKey(entry.key());
        if ((key == QLatin1String("ok") || key == QLatin1String("success") ||
             key == QLatin1String("succeeded")) &&
            entry.value().isBool() && entry.value().toBool())
            return true;
    }
    return false;
}

bool explicitlyConfirmsVerification(const QJsonValue& value,
                                    qsizetype depth = 0)
{
    if (depth > maximumTraversalDepth) return false;
    if (value.isArray())
    {
        for (const auto& entry : value.toArray())
            if (explicitlyConfirmsVerification(entry, depth + 1)) return true;
        return false;
    }
    if (!value.isObject()) return false;

    const auto object = value.toObject();
    for (auto entry = object.constBegin(); entry != object.constEnd(); ++entry)
    {
        const auto key = normalizedKey(entry.key());
        const auto valueText = entry.value().toString().trimmed().toLower();
        if ((key == QLatin1String("confirmation") ||
             key == QLatin1String("verification") ||
             key == QLatin1String("verificationstate")) &&
            (valueText == QLatin1String("verified") ||
             valueText == QLatin1String("confirmed")))
            return true;
        if ((entry.value().isObject() || entry.value().isArray()) &&
            explicitlyConfirmsVerification(entry.value(), depth + 1))
            return true;
    }
    return false;
}

bool hasStableResultLocator(const QJsonValue& value, qsizetype depth = 0,
                            bool parentContext = false)
{
    if (depth > maximumTraversalDepth) return false;
    if (value.isArray())
    {
        for (const auto& entry : value.toArray())
            if (hasStableResultLocator(entry, depth + 1, parentContext))
                return true;
        return false;
    }
    if (!value.isObject()) return false;

    const auto object = value.toObject();
    for (auto entry = object.constBegin(); entry != object.constEnd(); ++entry)
    {
        const auto itemParentContext =
            parentContext || isParentContextKey(entry.key());
        if (itemParentContext) continue;
        if (scalarText(entry.value()).isEmpty()) continue;
        const auto key = normalizedKey(entry.key());
        const auto isDiagnosticPath = key == QLatin1String("fieldpath") ||
                                      key == QLatin1String("modelpath") ||
                                      key == QLatin1String("instancepath") ||
                                      key == QLatin1String("schemapath");
        if (identityKeyScore(entry.key()) >= 0 ||
            (!isDiagnosticPath && (key.endsWith(QStringLiteral("path")) ||
                                   key.endsWith(QStringLiteral("uri")))))
            return true;
    }
    for (auto entry = object.constBegin(); entry != object.constEnd(); ++entry)
        if (hasStableResultLocator(
                entry.value(), depth + 1,
                parentContext || isParentContextKey(entry.key())))
            return true;
    return false;
}

bool targetTokensMatch(const QStringList& targetIds,
                       const QStringList& targetNames,
                       const QStringList& observedIds,
                       const QStringList& observedNames)
{
    if (!targetIds.isEmpty() && !observedIds.isEmpty())
        return intersects(targetIds, observedIds);
    return intersects(targetNames, observedNames);
}

bool structuredResultConfirmsTarget(const QJsonValue& payload,
                                    const QStringList& requestedIds,
                                    const QStringList& requestedNames,
                                    const QStringList& resultIds,
                                    const QStringList& resultNames)
{
    if (!explicitlyConfirmsSuccess(payload) || !hasStableResultLocator(payload))
        return false;
    if (requestedIds.isEmpty() && requestedNames.isEmpty())
        return !resultIds.isEmpty() || !resultNames.isEmpty();
    return targetTokensMatch(requestedIds, requestedNames, resultIds,
                             resultNames);
}

std::optional<QJsonValue> findTargetObject(const QJsonValue& value,
                                           const QStringList& targetIds,
                                           const QStringList& targetNames,
                                           qsizetype depth, qsizetype& visited)
{
    if (depth > maximumTraversalDepth || visited >= maximumVisitedValues)
        return std::nullopt;
    ++visited;
    if (value.isArray())
    {
        for (const auto& item : value.toArray())
        {
            auto found = findTargetObject(item, targetIds, targetNames,
                                          depth + 1, visited);
            if (found.has_value()) return found;
        }
        return std::nullopt;
    }
    if (!value.isObject()) return std::nullopt;

    QStringList directIds;
    QStringList directNames;
    const auto object = value.toObject();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        if (isParentContextKey(item.key())) continue;
        const auto scalar = scalarText(item.value());
        if (scalar.isEmpty()) continue;
        if (identityKeyScore(item.key()) >= 0)
            directIds.append(scalar);
        else if (nameKeyScore(item.key()) >= 0)
            directNames.append(scalar);
    }
    if (targetTokensMatch(targetIds, targetNames, directIds, directNames))
        return value;

    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        if (!item.value().isObject() && !item.value().isArray()) continue;
        auto found = findTargetObject(item.value(), targetIds, targetNames,
                                      depth + 1, visited);
        if (found.has_value()) return found;
    }
    return std::nullopt;
}

void collectResources(const QJsonValue& value, const QString& path,
                      const QString& serverId, const QString& tool,
                      int evidenceSequence, qsizetype depth, qsizetype& visited,
                      QList<AgentResourceRecord>& resources)
{
    if (depth > maximumTraversalDepth || visited >= maximumVisitedValues ||
        resources.size() >= maximumResources)
        return;
    ++visited;
    if (value.isArray())
    {
        const auto array = value.toArray();
        for (qsizetype index = 0;
             index < array.size() && resources.size() < maximumResources;
             ++index)
            collectResources(array.at(index),
                             path + QStringLiteral("[%1]").arg(index), serverId,
                             tool, evidenceSequence, depth + 1, visited,
                             resources);
        return;
    }
    if (!value.isObject()) return;

    const auto object = value.toObject();
    QString identity;
    QString identityField;
    QString parentId;
    QString parentField;
    QString name;
    QString kind;
    auto identityScore = -1;
    auto parentScore = -1;
    auto nameScore = -1;
    auto kindScore = -1;
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        const auto scalar = scalarText(item.value());
        if (scalar.isEmpty()) continue;
        const auto candidateIdentityScore = identityKeyScore(item.key());
        if (candidateIdentityScore > identityScore)
        {
            identity = scalar;
            identityField = item.key();
            identityScore = candidateIdentityScore;
        }
        const auto candidateParentScore = parentKeyScore(item.key());
        if (candidateParentScore > parentScore)
        {
            parentId = scalar;
            parentField = item.key();
            parentScore = candidateParentScore;
        }
        const auto candidateNameScore = nameKeyScore(item.key());
        if (candidateNameScore > nameScore)
        {
            name = scalar;
            nameScore = candidateNameScore;
        }
        const auto candidateKindScore = kindKeyScore(item.key());
        if (candidateKindScore > kindScore)
        {
            kind = scalar;
            kindScore = candidateKindScore;
        }
    }
    if (!identity.isEmpty())
        resources.append({serverId, kind, name, identity, identityField,
                          parentId, parentField, tool, path, evidenceSequence});

    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        if (!item.value().isObject() && !item.value().isArray()) continue;
        collectResources(item.value(), path + QLatin1Char('.') + item.key(),
                         serverId, tool, evidenceSequence, depth + 1, visited,
                         resources);
    }
}

QString actionKey(const agent::Action& action)
{
    return action.toolName + QLatin1Char('\n') +
           QString::fromUtf8(
               QJsonDocument(action.arguments).toJson(QJsonDocument::Compact));
}

void appendSemanticToken(QStringList& tokens, const QString& value,
                         bool includeAliases);

void collectSemanticTokens(const QJsonValue& value, QStringList& identities,
                           QStringList& names, qsizetype depth,
                           qsizetype& visited, bool includeAliases,
                           bool parentContext)
{
    if (depth > maximumTraversalDepth || visited >= maximumVisitedValues)
        return;
    ++visited;
    if (value.isArray())
    {
        for (const auto& item : value.toArray())
            collectSemanticTokens(item, identities, names, depth + 1, visited,
                                  includeAliases, parentContext);
        return;
    }
    if (!value.isObject()) return;
    const auto object = value.toObject();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        const auto itemParentContext =
            parentContext || isParentContextKey(item.key());
        const auto scalar = scalarText(item.value());
        if (!scalar.isEmpty() && !itemParentContext)
        {
            const auto normalized = normalizedKey(item.key());
            if (identityKeyScore(item.key()) >= 0 ||
                isJobIdentityKey(item.key()) ||
                normalized.endsWith(QStringLiteral("uri")))
                appendSemanticToken(identities, scalar, includeAliases);
            else if (nameKeyScore(item.key()) >= 0 ||
                     normalized.endsWith(QStringLiteral("path")))
                appendSemanticToken(names, scalar, includeAliases);
        }
        if (item.value().isObject() || item.value().isArray())
            collectSemanticTokens(item.value(), identities, names, depth + 1,
                                  visited, includeAliases, itemParentContext);
    }
}

QStringList uniqueTokens(QStringList values)
{
    values.removeAll(QString{});
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

bool intersects(const QStringList& left, const QStringList& right)
{
    QSet<QString> normalizedRight;
    for (const auto& value : right)
        normalizedRight.insert(comparableToken(value));
    for (const auto& value : left)
        if (normalizedRight.contains(comparableToken(value))) return true;
    return false;
}

void appendSemanticToken(QStringList& tokens, const QString& value,
                         bool includeAliases)
{
    if (tokens.size() >= maximumSemanticTokens) return;
    const auto token = bounded(value);
    if (token.isEmpty()) return;
    tokens.append(token);
    if (!includeAliases) return;
    const auto normalizedSlashes =
        QString(token).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const auto slash = normalizedSlashes.lastIndexOf(QLatin1Char('/'));
    if (tokens.size() < maximumSemanticTokens && slash >= 0 &&
        slash + 1 < normalizedSlashes.size())
        tokens.append(normalizedSlashes.mid(slash + 1));
}

QString findJobId(const QJsonValue& value, qsizetype depth, qsizetype& visited)
{
    if (depth > maximumTraversalDepth || visited >= maximumVisitedValues)
        return {};
    ++visited;
    if (value.isArray())
    {
        for (const auto& item : value.toArray())
        {
            const auto found = findJobId(item, depth + 1, visited);
            if (!found.isEmpty()) return found;
        }
        return {};
    }
    if (!value.isObject()) return {};
    const auto object = value.toObject();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
        if (isJobIdentityKey(item.key()))
        {
            const auto found = scalarText(item.value());
            if (!found.isEmpty()) return found;
        }
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        const auto found = findJobId(item.value(), depth + 1, visited);
        if (!found.isEmpty()) return found;
    }
    return {};
}

QString findStatus(const QJsonValue& value, qsizetype depth, qsizetype& visited)
{
    if (depth > maximumTraversalDepth || visited >= maximumVisitedValues)
        return {};
    ++visited;
    if (value.isArray())
    {
        for (const auto& item : value.toArray())
        {
            const auto found = findStatus(item, depth + 1, visited);
            if (!found.isEmpty()) return found;
        }
        return {};
    }
    if (!value.isObject()) return {};
    const auto object = value.toObject();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        const auto key = normalizedKey(item.key());
        if ((key == QLatin1String("status") || key == QLatin1String("state")) &&
            item.value().isString())
            return bounded(item.value().toString());
        if (key == QLatin1String("isrunning") && item.value().isBool())
            return item.value().toBool() ? QStringLiteral("running")
                                         : QStringLiteral("completed");
    }
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        const auto found = findStatus(item.value(), depth + 1, visited);
        if (!found.isEmpty()) return found;
    }
    return {};
}

QJsonObject resourceJson(const AgentResourceRecord& resource)
{
    QJsonObject result{
        {QStringLiteral("serverId"), resource.serverId},
        {QStringLiteral("stableId"), resource.stableId},
        {QStringLiteral("stableIdField"), resource.stableIdField},
        {QStringLiteral("sourceEvidenceSequence"),
         resource.sourceEvidenceSequence}};
    for (const auto& item :
         {std::pair{QStringLiteral("kind"), resource.kind},
          std::pair{QStringLiteral("name"), resource.name},
          std::pair{QStringLiteral("parentId"), resource.parentId},
          std::pair{QStringLiteral("parentIdField"), resource.parentIdField},
          std::pair{QStringLiteral("sourceTool"), resource.sourceTool},
          std::pair{QStringLiteral("sourcePath"), resource.sourcePath}})
        if (!item.second.isEmpty()) result.insert(item.first, item.second);
    return result;
}

QJsonObject jobJson(const AgentJobRecord& job)
{
    QJsonObject result{
        {QStringLiteral("serverId"), job.serverId},
        {QStringLiteral("tool"), job.tool},
        {QStringLiteral("jobId"), job.jobId},
        {QStringLiteral("status"), job.status},
        {QStringLiteral("terminal"), job.terminal},
        {QStringLiteral("lastPollAtMs"), job.lastPollAtMs},
        {QStringLiteral("sourceEvidenceSequence"), job.sourceEvidenceSequence}};
    if (!job.pollArguments.isEmpty())
        result.insert(QStringLiteral("pollArguments"), job.pollArguments);
    return result;
}

QJsonObject verificationJson(const AgentVerificationRecord& verification,
                             bool includeExpectedValues = false)
{
    QJsonArray ids;
    for (const auto& value : verification.targetIds)
        ids.append(value);
    QJsonArray names;
    for (const auto& value : verification.targetNames)
        names.append(value);
    QJsonArray expectedEffectFields;
    QJsonArray expectedEffects;
    for (const auto& expectation : verification.expectedEffects)
    {
        expectedEffectFields.append(expectation.field);
        if (includeExpectedValues)
            expectedEffects.append(QJsonObject{
                {QStringLiteral("field"), expectation.field},
                {QStringLiteral("value"), expectation.expectedValue}});
    }
    QJsonArray matchedEffectFields;
    for (const auto& field : verification.matchedEffectFields)
        matchedEffectFields.append(field);
    QJsonArray missingEffectFields;
    for (const auto& field : verification.missingEffectFields)
        missingEffectFields.append(field);
    QJsonArray mismatchedEffectFields;
    for (const auto& field : verification.mismatchedEffectFields)
        mismatchedEffectFields.append(field);
    QJsonObject result{
        {QStringLiteral("taskId"), verification.taskId},
        {QStringLiteral("serverId"), verification.serverId},
        {QStringLiteral("mutationEvidenceSequence"),
         verification.mutationEvidenceSequence},
        {QStringLiteral("tool"), verification.tool},
        {QStringLiteral("state"), verification.state},
        {QStringLiteral("targetIds"), ids},
        {QStringLiteral("targetNames"), names},
        {QStringLiteral("expectedEffectFields"), expectedEffectFields},
        {QStringLiteral("matchedEffectFields"), matchedEffectFields},
        {QStringLiteral("missingEffectFields"), missingEffectFields},
        {QStringLiteral("mismatchedEffectFields"), mismatchedEffectFields}};
    if (includeExpectedValues)
        result.insert(QStringLiteral("expectedEffects"), expectedEffects);
    if (!verification.detail.isEmpty())
        result.insert(QStringLiteral("detail"), verification.detail);
    if (verification.verificationEvidenceSequence > 0)
        result.insert(QStringLiteral("verificationEvidenceSequence"),
                      verification.verificationEvidenceSequence);
    return result;
}
}  // namespace

void AgentLedger::clear()
{
    resources_.clear();
    jobs_.clear();
    verifications_.clear();
}

void AgentLedger::recordToolResult(const QString& taskId, int evidenceSequence,
                                   const agent::Action& action,
                                   const agent::ToolResult& result,
                                   ToolOperationKind operationKind)
{
    const auto payload = resultPayload(result);
    QList<AgentResourceRecord> extractedResources;
    qsizetype visited = 0;
    collectResources(payload, QStringLiteral("result"), result.serverId,
                     action.toolName, evidenceSequence, 0, visited,
                     extractedResources);

    for (const auto& resource : extractedResources)
    {
        auto existing =
            std::find_if(resources_.begin(), resources_.end(),
                         [&resource](const AgentResourceRecord& candidate)
                         {
                             return candidate.serverId == resource.serverId &&
                                    comparableToken(candidate.stableId) ==
                                        comparableToken(resource.stableId);
                         });
        if (existing == resources_.end())
        {
            if (resources_.size() < maximumResources)
                resources_.append(resource);
            continue;
        }
        if (!resource.kind.isEmpty()) existing->kind = resource.kind;
        if (!resource.name.isEmpty()) existing->name = resource.name;
        if (!resource.parentId.isEmpty())
        {
            existing->parentId = resource.parentId;
            existing->parentIdField = resource.parentIdField;
        }
        existing->sourceTool = resource.sourceTool;
        existing->sourcePath = resource.sourcePath;
        existing->sourceEvidenceSequence = evidenceSequence;
    }

    QStringList resultIds;
    QStringList resultNames;
    for (const auto& resource : extractedResources)
    {
        resultIds.append(resource.stableId);
        if (!resource.name.isEmpty()) resultNames.append(resource.name);
    }
    visited = 0;
    collectSemanticTokens(payload, resultIds, resultNames, 0, visited, false,
                          false);
    resultIds = uniqueTokens(std::move(resultIds));
    resultNames = uniqueTokens(std::move(resultNames));
    QStringList actionIds;
    QStringList actionNames;
    visited = 0;
    collectSemanticTokens(action.arguments, actionIds, actionNames, 0, visited,
                          true, false);
    actionIds = uniqueTokens(std::move(actionIds));
    actionNames = uniqueTokens(std::move(actionNames));

    const auto inProgress = toolResultIndicatesInProgress(result);
    const auto key = actionKey(action);
    visited = 0;
    auto jobId = findJobId(payload, 0, visited);
    visited = 0;
    auto status = findStatus(payload, 0, visited);
    auto existingJob = std::find_if(jobs_.begin(), jobs_.end(),
                                    [&key](const AgentJobRecord& candidate)
                                    { return candidate.pollKey == key; });
    if (existingJob == jobs_.end() && !jobId.isEmpty())
        existingJob =
            std::find_if(jobs_.begin(), jobs_.end(),
                         [&result, &jobId](const AgentJobRecord& candidate)
                         {
                             return candidate.serverId == result.serverId &&
                                    candidate.jobId == jobId;
                         });
    if (inProgress || existingJob != jobs_.end() || !jobId.isEmpty())
    {
        if (status.isEmpty())
            status = inProgress       ? QStringLiteral("in_progress")
                     : result.isError ? QStringLiteral("failed")
                                      : QStringLiteral("completed");
        if (jobId.isEmpty())
            jobId = existingJob != jobs_.end() ? existingJob->jobId
                                               : result.requestId;
        const auto terminal = !inProgress;
        if (existingJob == jobs_.end())
        {
            if (jobs_.size() < maximumJobs)
                jobs_.append({result.serverId, action.toolName, jobId, status,
                              terminal, QDateTime::currentMSecsSinceEpoch(),
                              boundedArguments(action.arguments),
                              evidenceSequence, key});
        }
        else
        {
            existingJob->jobId = jobId;
            existingJob->status = status;
            existingJob->terminal = terminal;
            existingJob->lastPollAtMs = QDateTime::currentMSecsSinceEpoch();
            existingJob->pollArguments = boundedArguments(action.arguments);
            existingJob->sourceEvidenceSequence = evidenceSequence;
        }
    }

    for (auto& verification : verifications_)
    {
        if (verification.taskId != taskId ||
            verification.serverId != result.serverId)
            continue;
        if (verification.state == QLatin1String("verified")) continue;
        const auto& observedIds = resultIds.isEmpty() ? actionIds : resultIds;
        const auto& observedNames =
            resultNames.isEmpty() ? actionNames : resultNames;
        if (!targetTokensMatch(verification.targetIds, verification.targetNames,
                               observedIds, observedNames))
            continue;
        if (operationKind != ToolOperationKind::ReadOnly) continue;
        if (result.isError)
        {
            verification.state = QStringLiteral("failed");
            verification.detail = QStringLiteral("The read-back tool failed.");
            verification.verificationEvidenceSequence = evidenceSequence;
        }
        else if (!inProgress)
        {
            auto effectPayload = payload;
            visited = 0;
            const auto targetObject =
                findTargetObject(payload, verification.targetIds,
                                 verification.targetNames, 0, visited);
            if (targetObject.has_value()) effectPayload = *targetObject;
            const auto effectVerification = ToolEffectVerifier::verify(
                verification.expectedEffects, effectPayload);
            verification.matchedEffectFields = effectVerification.matchedFields;
            verification.missingEffectFields = effectVerification.missingFields;
            verification.mismatchedEffectFields =
                effectVerification.mismatchedFields;
            switch (effectVerification.status)
            {
                case ToolEffectVerificationStatus::Verified:
                    verification.state = QStringLiteral("verified");
                    verification.detail.clear();
                    break;
                case ToolEffectVerificationStatus::Incomplete:
                    verification.state = QStringLiteral("pending");
                    verification.detail = QStringLiteral(
                        "The read-back did not expose every expected effect "
                        "field.");
                    break;
                case ToolEffectVerificationStatus::Mismatch:
                    verification.state = QStringLiteral("failed");
                    verification.detail = QStringLiteral(
                        "The read-back returned a different value for one or "
                        "more expected effect fields.");
                    break;
            }
            verification.verificationEvidenceSequence = evidenceSequence;
        }
    }

    const auto uncertainMutationFailure =
        result.isError &&
        result.sideEffectState == agent::ToolSideEffectState::Uncertain;
    if (operationKind != ToolOperationKind::Mutation ||
        (result.isError && !uncertainMutationFailure) || inProgress ||
        verifications_.size() >= maximumVerifications)
        return;

    const auto targetIds = actionIds.isEmpty() ? resultIds : actionIds;
    const auto targetNames = actionNames.isEmpty() ? resultNames : actionNames;
    const auto expectedEffects =
        ToolEffectVerifier::captureExpectations(action.arguments);
    const auto selfTargetVerified = structuredResultConfirmsTarget(
        payload, actionIds, actionNames, resultIds, resultNames);
    const auto selfEffectVerification =
        ToolEffectVerifier::verify(expectedEffects, payload);
    const auto providerVerified = !result.isError && selfTargetVerified &&
                                  selfEffectVerification.status !=
                                      ToolEffectVerificationStatus::Mismatch &&
                                  explicitlyConfirmsVerification(payload);
    const auto selfVerified =
        !result.isError && selfTargetVerified &&
        (providerVerified || selfEffectVerification.status ==
                                 ToolEffectVerificationStatus::Verified);
    const auto hasVerificationTarget =
        !targetIds.isEmpty() || !targetNames.isEmpty();
    AgentVerificationRecord verification;
    verification.taskId = taskId;
    verification.serverId = result.serverId;
    verification.mutationEvidenceSequence = evidenceSequence;
    verification.tool = action.toolName;
    verification.state = uncertainMutationFailure ? QStringLiteral("uncertain")
                         : selfVerified           ? QStringLiteral("verified")
                         : !hasVerificationTarget
                             ? QStringLiteral("unavailable")
                             : QStringLiteral("pending");
    verification.targetIds = targetIds;
    verification.targetNames = targetNames;
    verification.expectedEffects = expectedEffects;
    verification.matchedEffectFields = selfEffectVerification.matchedFields;
    if (!providerVerified)
    {
        verification.missingEffectFields = selfEffectVerification.missingFields;
        verification.mismatchedEffectFields =
            selfEffectVerification.mismatchedFields;
    }
    if (uncertainMutationFailure)
        verification.detail = QStringLiteral(
            "The mutation was dispatched, but its final outcome is unknown.");
    else if (!providerVerified && selfTargetVerified &&
             selfEffectVerification.status ==
                 ToolEffectVerificationStatus::Incomplete)
        verification.detail = QStringLiteral(
            "The mutation result did not expose every expected effect field.");
    else if (!providerVerified && selfTargetVerified &&
             selfEffectVerification.status ==
                 ToolEffectVerificationStatus::Mismatch)
        verification.detail = QStringLiteral(
            "The mutation result did not match the expected effect fields.");
    verification.verificationEvidenceSequence =
        selfVerified ? evidenceSequence : 0;
    verifications_.append(std::move(verification));
}

const QList<AgentResourceRecord>& AgentLedger::resources() const
{
    return resources_;
}

const QList<AgentJobRecord>& AgentLedger::jobs() const
{
    return jobs_;
}

const QList<AgentVerificationRecord>& AgentLedger::verifications() const
{
    return verifications_;
}

bool AgentLedger::hasUnresolvedVerification() const
{
    return std::any_of(verifications_.cbegin(), verifications_.cend(),
                       [](const AgentVerificationRecord& verification)
                       { return !verificationResolved(verification.state); });
}

bool AgentLedger::evidenceRequiresVerification(int sequence) const
{
    const auto verification = std::find_if(
        verifications_.cbegin(), verifications_.cend(),
        [sequence](const AgentVerificationRecord& candidate)
        { return candidate.mutationEvidenceSequence == sequence; });
    return verification != verifications_.cend() &&
           !verificationResolved(verification->state);
}

QList<int> AgentLedger::acceptUnresolvedVerification(
    const QString& taskId, int mutationEvidenceSequence)
{
    QList<int> accepted;
    if (mutationEvidenceSequence <= 0) return accepted;
    for (auto& verification : verifications_)
    {
        if (verificationResolved(verification.state) ||
            (!taskId.isEmpty() && verification.taskId != taskId) ||
            (mutationEvidenceSequence > 0 &&
             verification.mutationEvidenceSequence != mutationEvidenceSequence))
            continue;
        verification.state = QStringLiteral("accepted");
        verification.detail = QStringLiteral(
            "The user accepted this result without deterministic "
            "read-back verification.");
        accepted.append(verification.mutationEvidenceSequence);
    }
    return accepted;
}

QList<int> AgentLedger::attestUnresolvedVerification(
    const QString& taskId, int mutationEvidenceSequence,
    const QString& userEvidence)
{
    QList<int> attested;
    const auto evidence = userEvidence.trimmed();
    if (evidence.isEmpty() || mutationEvidenceSequence <= 0) return attested;
    const auto digest = QString::fromLatin1(
        QCryptographicHash::hash(evidence.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
    for (auto& verification : verifications_)
    {
        if (verificationResolved(verification.state) ||
            (!taskId.isEmpty() && verification.taskId != taskId) ||
            (mutationEvidenceSequence > 0 &&
             verification.mutationEvidenceSequence != mutationEvidenceSequence))
            continue;
        verification.state = QStringLiteral("attested");
        verification.detail =
            QStringLiteral(
                "The user supplied temporary evidence (sha256:%1, %2 "
                "characters); deterministic read-back was not available.")
                .arg(digest)
                .arg(evidence.size());
        attested.append(verification.mutationEvidenceSequence);
    }
    return attested;
}

QString AgentLedger::unresolvedVerificationReason(const QString& taskId) const
{
    const auto verification = std::find_if(
        verifications_.cbegin(), verifications_.cend(),
        [&taskId](const AgentVerificationRecord& candidate)
        {
            return !verificationResolved(candidate.state) &&
                   (taskId.isEmpty() || candidate.taskId == taskId);
        });
    if (verification == verifications_.cend()) return {};
    if (verification->state == QLatin1String("unavailable"))
        return QStringLiteral(
                   "Mutation evidence %1 has no stable identifier for an "
                   "independent read-back. Report this verification "
                   "limitation instead of claiming confirmed state.")
            .arg(verification->mutationEvidenceSequence);
    if (verification->state == QLatin1String("failed"))
    {
        if (!verification->mismatchedEffectFields.isEmpty())
            return QStringLiteral(
                       "The read-back for mutation evidence %1 returned "
                       "different values for expected field(s): %2. Inspect "
                       "the same target again before completion.")
                .arg(verification->mutationEvidenceSequence)
                .arg(verification->mismatchedEffectFields.join(
                    QStringLiteral(", ")));
        return QStringLiteral(
                   "The read-back for mutation evidence %1 failed. Use the "
                   "known identifiers to inspect it again before completion.")
            .arg(verification->mutationEvidenceSequence);
    }
    if (!verification->missingEffectFields.isEmpty())
        return QStringLiteral(
                   "Mutation evidence %1 is still unverified because the "
                   "read-back omitted expected field(s): %2. Inspect the "
                   "same target with those fields before completion.")
            .arg(verification->mutationEvidenceSequence)
            .arg(verification->missingEffectFields.join(QStringLiteral(", ")));
    return QStringLiteral(
               "Mutation evidence %1 is awaiting read-back verification. "
               "Reuse its recorded identifiers with an existing inspection "
               "tool before completion.")
        .arg(verification->mutationEvidenceSequence);
}

QJsonObject AgentLedger::unresolvedVerificationContext(
    const QString& taskId) const
{
    const auto verification = std::find_if(
        verifications_.cbegin(), verifications_.cend(),
        [&taskId](const AgentVerificationRecord& candidate)
        {
            return !verificationResolved(candidate.state) &&
                   (taskId.isEmpty() || candidate.taskId == taskId);
        });
    return verification == verifications_.cend()
               ? QJsonObject{}
               : verificationJson(*verification, true);
}

QJsonObject AgentLedger::snapshot() const
{
    QJsonArray resources;
    for (const auto& resource : resources_)
        resources.append(resourceJson(resource));
    QJsonArray jobs;
    for (const auto& job : jobs_)
        jobs.append(jobJson(job));
    QJsonArray verifications;
    for (const auto& verification : verifications_)
        verifications.append(verificationJson(verification));
    return {{QStringLiteral("resources"), resources},
            {QStringLiteral("jobs"), jobs},
            {QStringLiteral("verifications"), verifications},
            {QStringLiteral("hasUnresolvedVerification"),
             hasUnresolvedVerification()}};
}
}  // namespace qtllm::application
