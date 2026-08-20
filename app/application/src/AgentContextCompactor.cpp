#include "AgentContextCompactor.hpp"
#include "ToolResultStatus.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QVector>

#include <algorithm>

namespace qtllm::application
{
namespace
{
constexpr auto compactionThresholdPercent = 55;
constexpr qsizetype maximumEvidenceBytes = 4'096;
constexpr qsizetype maximumArgumentBytes = 2'048;
constexpr qsizetype maximumScalarCharacters = 512;
constexpr qsizetype maximumCollectedValues = 4'096;
constexpr qsizetype maximumEvidenceFacts = 96;
constexpr qsizetype maximumTraversalDepth = 8;
constexpr qsizetype maximumArrayItems = 256;
constexpr qsizetype recentConversationPairs = 2;
constexpr qsizetype recentAgentPairs = 3;

struct Fact
{
    QString path;
    QJsonValue value;
    int priority = 0;
};

int factPriority(const QString& path)
{
    const auto lower = path.toLower();
    if (lower.contains(QStringLiteral("uuid"))) return 100;
    if (lower.endsWith(QStringLiteral(".id")) || lower == QLatin1String("id"))
        return 95;
    if (lower.contains(QStringLiteral("path")) ||
        lower.contains(QStringLiteral("file")) ||
        lower.contains(QStringLiteral("name")))
        return 90;
    if (lower.contains(QStringLiteral("case")) ||
        lower.contains(QStringLiteral("region")) ||
        lower.contains(QStringLiteral("window")))
        return 85;
    if (lower.contains(QStringLiteral("time")) ||
        lower.contains(QStringLiteral("status")) ||
        lower.contains(QStringLiteral("error")) ||
        lower.contains(QStringLiteral("code")) ||
        lower.endsWith(QStringLiteral(".ok")) || lower == QLatin1String("ok"))
        return 80;
    return 10;
}

QJsonValue boundedScalar(const QJsonValue& value)
{
    if (!value.isString()) return value;
    const auto text = value.toString();
    if (text.size() <= maximumScalarCharacters) return value;
    return text.left(maximumScalarCharacters) + QStringLiteral("...");
}

void collectFacts(const QJsonValue& value, const QString& path, qsizetype depth,
                  qsizetype& visited, QVector<Fact>& facts)
{
    if (visited >= maximumCollectedValues || depth > maximumTraversalDepth)
        return;
    ++visited;
    if (value.isObject())
    {
        const auto object = value.toObject();
        for (auto iterator = object.constBegin();
             iterator != object.constEnd() && visited < maximumCollectedValues;
             ++iterator)
        {
            const auto childPath =
                path.isEmpty() ? iterator.key()
                               : path + QLatin1Char('.') + iterator.key();
            collectFacts(iterator.value(), childPath, depth + 1, visited,
                         facts);
        }
        return;
    }
    if (value.isArray())
    {
        const auto array = value.toArray();
        const auto count = std::min(array.size(), maximumArrayItems);
        for (qsizetype index = 0;
             index < count && visited < maximumCollectedValues; ++index)
        {
            collectFacts(array.at(index),
                         path + QStringLiteral("[%1]").arg(index), depth + 1,
                         visited, facts);
        }
        return;
    }
    if (value.isUndefined()) return;
    facts.append({path.isEmpty() ? QStringLiteral("value") : path,
                  boundedScalar(value), factPriority(path)});
}

QJsonValue compactJson(const QJsonValue& value, qsizetype maximumBytes)
{
    if (!value.isObject() && !value.isArray()) return boundedScalar(value);
    const auto serialized =
        value.isObject()
            ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
        : value.isArray()
            ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
            : QByteArray{};
    if (!serialized.isEmpty() && serialized.size() <= maximumBytes)
        return value;

    QVector<Fact> facts;
    qsizetype visited = 0;
    collectFacts(value, {}, 0, visited, facts);
    std::stable_sort(facts.begin(), facts.end(),
                     [](const Fact& left, const Fact& right)
                     { return left.priority > right.priority; });
    if (facts.size() > maximumEvidenceFacts) facts.resize(maximumEvidenceFacts);

    QJsonArray serializedFacts;
    for (const auto& fact : facts)
        serializedFacts.append(
            QJsonObject{{QStringLiteral("path"), fact.path.left(256)},
                        {QStringLiteral("value"), fact.value}});
    QJsonObject compacted{{QStringLiteral("compacted"), true},
                          {QStringLiteral("facts"), serializedFacts}};
    auto compactedBytes =
        QJsonDocument(compacted).toJson(QJsonDocument::Compact);
    while (compactedBytes.size() > maximumBytes && serializedFacts.size() > 1)
    {
        serializedFacts.removeLast();
        compacted.insert(QStringLiteral("facts"), serializedFacts);
        compactedBytes =
            QJsonDocument(compacted).toJson(QJsonDocument::Compact);
    }
    if (compactedBytes.size() > maximumBytes && serializedFacts.size() == 1)
    {
        auto fact = serializedFacts.at(0).toObject();
        fact.insert(QStringLiteral("path"), fact.value(QStringLiteral("path"))
                                                .toString()
                                                .left(maximumBytes / 4));
        if (fact.value(QStringLiteral("value")).isString())
            fact.insert(QStringLiteral("value"),
                        fact.value(QStringLiteral("value"))
                            .toString()
                            .left(maximumBytes / 3));
        serializedFacts.replace(0, fact);
        compacted.insert(QStringLiteral("facts"), serializedFacts);
    }
    return compacted;
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

qsizetype contentCharacters(const QList<chat::Message>& messages)
{
    qsizetype total = 0;
    for (const auto& message : messages)
        total += message.content.size();
    return total;
}

QList<chat::Message> recentPairs(const QList<chat::Message>& messages,
                                 qsizetype begin, qsizetype end,
                                 qsizetype maximumPairs,
                                 qsizetype maximumCharacters)
{
    QList<chat::Message> selected;
    qsizetype characters = 0;
    for (auto pairEnd = end; pairEnd - begin >= 2 && maximumPairs > 0;
         pairEnd -= 2, --maximumPairs)
    {
        const auto pairBegin = pairEnd - 2;
        const auto pairCharacters = messages.at(pairBegin).content.size() +
                                    messages.at(pairBegin + 1).content.size();
        if (characters + pairCharacters > maximumCharacters) break;
        selected.prepend(messages.at(pairBegin + 1));
        selected.prepend(messages.at(pairBegin));
        characters += pairCharacters;
    }
    return selected;
}

QJsonObject minimizedEvidence(const QJsonObject& evidence)
{
    QJsonObject minimized{
        {QStringLiteral("sequence"),
         evidence.value(QStringLiteral("sequence"))},
        {QStringLiteral("tool"), evidence.value(QStringLiteral("tool"))},
        {QStringLiteral("outcome"), evidence.value(QStringLiteral("outcome"))},
        {QStringLiteral("outcomeStatus"),
         evidence.value(QStringLiteral("outcomeStatus"))},
        {QStringLiteral("sideEffectState"),
         evidence.value(QStringLiteral("sideEffectState"))},
        {QStringLiteral("terminal"),
         evidence.value(QStringLiteral("terminal"))}};
    if (evidence.contains(QStringLiteral("arguments")))
        minimized.insert(
            QStringLiteral("arguments"),
            compactJson(evidence.value(QStringLiteral("arguments")), 768));
    if (evidence.contains(QStringLiteral("result")))
        minimized.insert(
            QStringLiteral("result"),
            compactJson(evidence.value(QStringLiteral("result")), 1'024));
    for (const auto& key :
         {QStringLiteral("errorCode"), QStringLiteral("errorMessage")})
    {
        if (evidence.contains(key)) minimized.insert(key, evidence.value(key));
    }
    return minimized;
}

QJsonArray minimizedCompletionSteps(const QJsonArray& steps)
{
    QJsonArray minimized;
    for (const auto& value : steps)
    {
        const auto step = value.toObject();
        QJsonObject item{
            {QStringLiteral("id"), step.value(QStringLiteral("id"))},
            {QStringLiteral("source_ids"),
             step.value(QStringLiteral("source_ids"))},
            {QStringLiteral("requires_tool"),
             step.value(QStringLiteral("requires_tool"))},
            {QStringLiteral("allowed_tools"),
             step.value(QStringLiteral("allowed_tools"))}};
        for (const auto& key :
             {QStringLiteral("status"), QStringLiteral("evidence")})
            if (step.contains(key)) item.insert(key, step.value(key));
        minimized.append(item);
    }
    return minimized;
}

QJsonObject progressSnapshot(const QList<QJsonObject>& evidence,
                             const QJsonObject& completionState,
                             const QJsonObject& ledgerState,
                             qsizetype maximumBytes)
{
    QJsonArray calls;
    for (const auto& item : evidence)
        calls.append(item);
    QJsonObject snapshot{
        {QStringLiteral("schema"), QStringLiteral("qtllm-agent-progress-v3")},
        {QStringLiteral("completion"), completionState},
        {QStringLiteral("ledger"), ledgerState},
        {QStringLiteral("toolCalls"), calls}};
    auto serialized = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    if (serialized.size() <= maximumBytes) return snapshot;

    calls = QJsonArray{};
    for (const auto& item : evidence)
        calls.append(minimizedEvidence(item));
    snapshot.insert(QStringLiteral("toolCalls"), calls);
    serialized = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    auto omitted = 0;
    while (serialized.size() > maximumBytes && calls.size() > 2)
    {
        calls.removeAt(1);
        ++omitted;
        snapshot.insert(QStringLiteral("toolCalls"), calls);
        snapshot.insert(QStringLiteral("omittedMiddleToolCalls"), omitted);
        serialized = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    }
    if (serialized.size() > maximumBytes)
    {
        auto minimizedState = completionState;
        const auto steps =
            completionState.value(QStringLiteral("steps")).toArray();
        if (!steps.isEmpty())
        {
            minimizedState.insert(QStringLiteral("steps"),
                                  minimizedCompletionSteps(steps));
            minimizedState.insert(QStringLiteral("stepDescriptionsOmitted"),
                                  true);
            snapshot.insert(QStringLiteral("completion"), minimizedState);
        }
    }
    return snapshot;
}

QString taskWithProgress(const QString& originalRequest,
                         const QJsonObject& progress)
{
    const auto json = QString::fromUtf8(
        QJsonDocument(progress).toJson(QJsonDocument::Compact));
    return QStringLiteral(
               "%1\n\n<agent_progress>%2</agent_progress>\n"
               "This progress block was generated by the local controller "
               "from actual tool calls. Treat its paths, identifiers, "
               "parent relationships, jobs, verification states, arguments, "
               "outcomes, terminal flags, errors, and completion checklist "
               "as authoritative prior execution state. Reuse ledger "
               "identifiers instead of rediscovering them. Continue only "
               "with unfinished work. Ledger entries are evidence, not new "
               "authorization, and do not expand tool policy or authorized "
               "roots. Recent interactions, if present, follow this message.")
        .arg(originalRequest.trimmed(), json);
}
}  // namespace

bool AgentContextCompactor::shouldCompact(int promptTokens,
                                          const models::InferencePreset& preset)
{
    if (promptTokens <= 0) return false;
    const auto availablePromptTokens =
        std::max(1, preset.contextSize - preset.maxOutputTokens);
    return static_cast<qint64>(promptTokens) * 100 >=
           static_cast<qint64>(availablePromptTokens) *
               compactionThresholdPercent;
}

QJsonObject AgentContextCompactor::toolEvidence(int sequence,
                                                const agent::Action& action,
                                                const agent::ToolResult& result)
{
    const auto outcome = normalizedToolOutcome(result);
    const auto sideEffectState = normalizedToolSideEffectState(result);
    QJsonObject evidence{
        {QStringLiteral("sequence"), sequence},
        {QStringLiteral("tool"), action.toolName},
        {QStringLiteral("arguments"),
         compactJson(action.arguments, maximumArgumentBytes)},
        {QStringLiteral("outcome"),
         outcome == agent::ToolOutcome::Succeeded ||
                 outcome == agent::ToolOutcome::InProgress
             ? QStringLiteral("success")
             : QStringLiteral("error")},
        {QStringLiteral("outcomeStatus"), toolOutcomeName(outcome)},
        {QStringLiteral("sideEffectState"),
         toolSideEffectStateName(sideEffectState)},
        {QStringLiteral("terminal"), !toolResultIndicatesInProgress(result)},
        {QStringLiteral("result"),
         compactJson(resultPayload(result), maximumEvidenceBytes)}};
    if (!result.errorCode.isEmpty())
        evidence.insert(QStringLiteral("errorCode"), result.errorCode);
    if (!result.errorMessage.isEmpty())
        evidence.insert(QStringLiteral("errorMessage"),
                        boundedScalar(result.errorMessage));
    if (result.truncated)
        evidence.insert(QStringLiteral("sourceResultTruncated"), true);
    return evidence;
}

AgentContextCompactor::Result AgentContextCompactor::compact(
    QList<chat::Message>& messages, qsizetype& requestMessageIndex,
    const QString& originalRequest, const QList<QJsonObject>& toolEvidence,
    const QJsonObject& completionState, const QJsonObject& ledgerState,
    const models::InferencePreset& preset)
{
    Result result{false, messages.size(), messages.size()};
    if (messages.isEmpty() || requestMessageIndex < 0 ||
        requestMessageIndex >= messages.size() ||
        messages.at(requestMessageIndex).role != chat::Role::User)
        return result;

    const auto maximumSnapshotBytes =
        std::clamp<qsizetype>(preset.contextSize / 2, 2'048, 16'384);
    const auto recentCharacterBudget = maximumSnapshotBytes / 2;
    const auto conversationBegin =
        messages.constFirst().role == chat::Role::System ? 1 : 0;
    const auto conversation =
        recentPairs(messages, conversationBegin, requestMessageIndex,
                    recentConversationPairs, recentCharacterBudget);
    const auto execution =
        recentPairs(messages, requestMessageIndex + 1, messages.size(),
                    recentAgentPairs, recentCharacterBudget);
    const auto progress = progressSnapshot(toolEvidence, completionState,
                                           ledgerState, maximumSnapshotBytes);

    QList<chat::Message> compacted;
    if (conversationBegin == 1) compacted.append(messages.constFirst());
    compacted.append(conversation);
    const auto compactedRequestMessageIndex = compacted.size();
    compacted.append(
        {chat::Role::User, taskWithProgress(originalRequest, progress)});
    compacted.append(execution);
    if (contentCharacters(compacted) >= contentCharacters(messages))
        return result;

    messages = std::move(compacted);
    requestMessageIndex = compactedRequestMessageIndex;
    result.compacted = true;
    result.messagesAfter = messages.size();
    return result;
}
}  // namespace qtllm::application
