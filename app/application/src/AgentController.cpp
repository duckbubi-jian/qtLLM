#include "AgentController.hpp"

#include "AgentContextCompactor.hpp"
#include "AgentPromptBuilder.hpp"
#include "AgentRunMetrics.hpp"
#include "ToolResultStatus.hpp"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace qtllm::application
{
namespace
{
constexpr auto maximumDecisionBytes = 65'536;
constexpr auto minimumDecisionTokens = 256;
constexpr auto runTimeoutMilliseconds = 120'000;
constexpr auto minimumPollIntervalMilliseconds = 1'000;
constexpr auto maximumCompletionReviewsPerEvidenceRevision = 2;
constexpr auto maximumContractFailuresPerTarget = 2;
constexpr auto maximumConsecutiveDiscoveryCalls = 4;
constexpr qsizetype maximumDetectedCycleLength = 4;
constexpr qsizetype maximumLoggedEventDataBytes = 4'096;
constexpr qsizetype maximumInvalidatedResourceIds = 128;
constexpr qsizetype maximumProgressActivities = 8;

QString unqualifiedToolName(const QString& qualifiedToolName)
{
    return qualifiedToolName.section(QLatin1Char('.'), -1).toLower();
}

bool isContextResetToolName(const QString& qualifiedToolName)
{
    static const QStringList names{
        QStringLiteral("close_case"), QStringLiteral("load_case"),
        QStringLiteral("new_case"), QStringLiteral("open_case"),
        QStringLiteral("switch_case")};
    return names.contains(unqualifiedToolName(qualifiedToolName));
}

bool opensContext(const QString& qualifiedToolName)
{
    static const QStringList names{QStringLiteral("load_case"),
                                   QStringLiteral("open_case"),
                                   QStringLiteral("switch_case")};
    return names.contains(unqualifiedToolName(qualifiedToolName));
}

bool closesContext(const QString& qualifiedToolName)
{
    return unqualifiedToolName(qualifiedToolName) ==
           QLatin1String("close_case");
}

bool explicitlyRequestsContextClose(const QString& request)
{
    const auto normalized = request.trimmed().toLower();
    static const QStringList negativePhrases{
        QStringLiteral("do not close"),
        QStringLiteral("don't close"),
        QStringLiteral("without closing"),
        QStringLiteral("keep open"),
        QStringLiteral("\u4e0d\u8981\u5173\u95ed"),
        QStringLiteral("\u522b\u5173\u95ed"),
        QStringLiteral("\u4fdd\u6301\u6253\u5f00")};
    for (const auto& phrase : negativePhrases)
        if (normalized.contains(phrase)) return false;

    static const QStringList closeTerms{
        QStringLiteral("close_case"),   QStringLiteral("close"),
        QStringLiteral("shut"),         QStringLiteral("\u5173\u95ed"),
        QStringLiteral("\u5173\u6389"), QStringLiteral("\u9000\u51fa")};
    return std::any_of(closeTerms.cbegin(), closeTerms.cend(),
                       [&normalized](const QString& term)
                       { return normalized.contains(term); });
}

bool isSingleContextOperationRequest(const QString& request,
                                     const QString& qualifiedToolName)
{
    const auto name = unqualifiedToolName(qualifiedToolName);
    if (name != QLatin1String("open_case") &&
        name != QLatin1String("load_case") &&
        name != QLatin1String("switch_case") &&
        name != QLatin1String("new_case") &&
        name != QLatin1String("close_case"))
        return false;

    const auto normalized = request.trimmed().toLower();
    auto nonEmptyLines = 0;
    for (const auto& line : normalized.split(QLatin1Char('\n')))
        if (!line.trimmed().isEmpty()) ++nonEmptyLines;
    if (nonEmptyLines > 1) return false;

    QStringList operationTerms;
    if (name == QLatin1String("new_case"))
        operationTerms = {QStringLiteral("new"), QStringLiteral("create"),
                          QStringLiteral("\u65b0\u5efa"),
                          QStringLiteral("\u521b\u5efa")};
    else if (name == QLatin1String("close_case"))
        operationTerms = {QStringLiteral("close"), QStringLiteral("shut"),
                          QStringLiteral("\u5173\u95ed"),
                          QStringLiteral("\u5173\u6389"),
                          QStringLiteral("\u9000\u51fa")};
    else
        operationTerms = {
            QStringLiteral("open"),         QStringLiteral("load"),
            QStringLiteral("switch"),       QStringLiteral("\u6253\u5f00"),
            QStringLiteral("\u52a0\u8f7d"), QStringLiteral("\u5207\u6362")};
    const auto mentionsOperation =
        std::any_of(operationTerms.cbegin(), operationTerms.cend(),
                    [&normalized](const QString& term)
                    { return normalized.contains(term); });
    if (!mentionsOperation) return false;

    static const QStringList additionalOperationTerms{
        QStringLiteral("open"),         QStringLiteral("load"),
        QStringLiteral("switch"),       QStringLiteral("new"),
        QStringLiteral("create"),       QStringLiteral("inspect"),
        QStringLiteral("list"),         QStringLiteral("check"),
        QStringLiteral("query"),        QStringLiteral("find"),
        QStringLiteral("configure"),    QStringLiteral("set "),
        QStringLiteral("edit"),         QStringLiteral("import"),
        QStringLiteral("run"),          QStringLiteral("solve"),
        QStringLiteral("simulate"),     QStringLiteral("read"),
        QStringLiteral("delete"),       QStringLiteral("close"),
        QStringLiteral("\u6253\u5f00"), QStringLiteral("\u52a0\u8f7d"),
        QStringLiteral("\u5207\u6362"), QStringLiteral("\u65b0\u5efa"),
        QStringLiteral("\u521b\u5efa"), QStringLiteral("\u67e5\u770b"),
        QStringLiteral("\u5217\u51fa"), QStringLiteral("\u68c0\u67e5"),
        QStringLiteral("\u67e5\u8be2"), QStringLiteral("\u914d\u7f6e"),
        QStringLiteral("\u8bbe\u7f6e"), QStringLiteral("\u7f16\u8f91"),
        QStringLiteral("\u5bfc\u5165"), QStringLiteral("\u8fd0\u884c"),
        QStringLiteral("\u6c42\u89e3"), QStringLiteral("\u8ba1\u7b97"),
        QStringLiteral("\u8bfb\u53d6"), QStringLiteral("\u5220\u9664"),
        QStringLiteral("\u5173\u95ed")};
    return std::none_of(
        additionalOperationTerms.cbegin(), additionalOperationTerms.cend(),
        [&normalized, &operationTerms](const QString& term)
        {
            return normalized.contains(term) && !operationTerms.contains(term);
        });
}

bool explicitlyRequestsExhaustiveDiscovery(const QString& request)
{
    const auto normalized = request.trimmed().toLower();
    static const QRegularExpression englishTerms(QStringLiteral(
        R"(\b(all|every|everything|entire|exhaustive)\b|complete\s+inventory)"));
    if (englishTerms.match(normalized).hasMatch()) return true;

    static const QStringList terms{QStringLiteral("\u5168\u90e8"),
                                   QStringLiteral("\u6240\u6709"),
                                   QStringLiteral("\u9010\u9879"),
                                   QStringLiteral("\u5b8c\u6574\u76d8\u70b9")};
    return std::any_of(terms.cbegin(), terms.cend(),
                       [&normalized](const QString& term)
                       { return normalized.contains(term); });
}

QString stringArgument(const QJsonObject& arguments,
                       const QStringList& candidateKeys)
{
    for (const auto& key : candidateKeys)
    {
        const auto value = arguments.value(key);
        if (value.isString() && !value.toString().trimmed().isEmpty())
            return value.toString().trimmed();
    }
    return {};
}

QString actionItemType(const agent::Action& action)
{
    return stringArgument(action.arguments, {QStringLiteral("item_type"),
                                             QStringLiteral("object_type"),
                                             QStringLiteral("model_type")});
}

QString actionSelector(const agent::Action& action)
{
    return stringArgument(
        action.arguments,
        {QStringLiteral("name_uuid"), QStringLiteral("object_uuid"),
         QStringLiteral("resource_uuid"), QStringLiteral("uuid"),
         QStringLiteral("id")});
}

QString contractFailureKey(const agent::Action& action)
{
    return action.toolName.toCaseFolded() + QLatin1Char('\n') +
           actionItemType(action).toCaseFolded() + QLatin1Char('\n') +
           actionSelector(action).toCaseFolded();
}

QString modelContractKey(const QString& serverId, const QString& itemType,
                         const QString& selector)
{
    return serverId.toCaseFolded() + QLatin1Char('\n') +
           itemType.toCaseFolded() + QLatin1Char('\n') +
           selector.toCaseFolded();
}

QJsonValue toolResultPayload(const agent::ToolResult& result);

bool isValidModelJsonPointer(const QString& path)
{
    if (path.isEmpty() || !path.startsWith(QLatin1Char('/'))) return false;
    for (qsizetype index = 0; index < path.size(); ++index)
    {
        if (path.at(index) != QLatin1Char('~')) continue;
        if (++index >= path.size() || (path.at(index) != QLatin1Char('0') &&
                                       path.at(index) != QLatin1Char('1')))
            return false;
    }
    return true;
}

void appendDescribedFieldPaths(const QJsonValue& value, QStringList& paths,
                               qsizetype depth = 0)
{
    if (depth > 12) return;
    if (value.isString())
    {
        const auto path = value.toString().trimmed();
        if (isValidModelJsonPointer(path) && !paths.contains(path))
            paths.append(path);
        return;
    }
    if (value.isArray())
    {
        for (const auto& entry : value.toArray())
            appendDescribedFieldPaths(entry, paths, depth + 1);
        return;
    }
    if (!value.isObject()) return;

    const auto object = value.toObject();
    const auto path = object.value(QStringLiteral("path")).toString().trimmed();
    if (isValidModelJsonPointer(path) && !paths.contains(path))
        paths.append(path);
    for (auto entry = object.constBegin(); entry != object.constEnd(); ++entry)
    {
        if (entry.key() == QLatin1String("path")) continue;
        appendDescribedFieldPaths(entry.value(), paths, depth + 1);
    }
}

QStringList describedFieldPaths(const agent::ToolResult& result)
{
    QStringList paths;
    const auto payload = toolResultPayload(result);
    if (!payload.isObject()) return paths;

    const auto collectFields = [&paths](const auto& self,
                                        const QJsonValue& value,
                                        qsizetype depth) -> void
    {
        if (depth > 10) return;
        if (value.isArray())
        {
            for (const auto& entry : value.toArray())
                self(self, entry, depth + 1);
            return;
        }
        if (!value.isObject()) return;
        const auto object = value.toObject();
        for (auto entry = object.constBegin(); entry != object.constEnd();
             ++entry)
        {
            const auto key = entry.key().toCaseFolded();
            if (key == QLatin1String("fields") ||
                key == QLatin1String("editable_fields") ||
                key == QLatin1String("editablefields") ||
                key == QLatin1String("field_paths") ||
                key == QLatin1String("fieldpaths"))
                appendDescribedFieldPaths(entry.value(), paths);
            else
                self(self, entry.value(), depth + 1);
        }
    };
    collectFields(collectFields, payload, 0);
    paths.sort(Qt::CaseSensitive);
    return paths;
}

QString summarizedAllowedPaths(const QStringList& paths,
                               const QString& rejectedPath)
{
    if (paths.isEmpty()) return {};
    auto candidatePath = rejectedPath;
    if (candidatePath.startsWith(QLatin1String("/changes/")))
        candidatePath = candidatePath.sliced(9);
    candidatePath.replace(QStringLiteral("~1"), QStringLiteral("/"));
    candidatePath.replace(QStringLiteral("~0"), QStringLiteral("~"));
    if (!candidatePath.startsWith(QLatin1Char('/')))
        candidatePath.prepend(QLatin1Char('/'));
    const auto leaf = candidatePath.section(QLatin1Char('/'), -1);

    QStringList relevant;
    for (const auto& path : paths)
        if (!leaf.isEmpty() && path.section(QLatin1Char('/'), -1) == leaf)
            relevant.append(path);
    if (relevant.isEmpty()) relevant = paths;
    constexpr qsizetype maximumDisplayedPaths = 24;
    const auto omitted =
        relevant.size() - std::min(relevant.size(), maximumDisplayedPaths);
    relevant =
        relevant.sliced(0, std::min(relevant.size(), maximumDisplayedPaths));
    auto summary = relevant.join(QStringLiteral(", "));
    if (omitted > 0)
        summary += QStringLiteral(
                       " (and %1 more; call describe_model again "
                       "to inspect them)")
                       .arg(omitted);
    return summary;
}

bool isContractFailure(const agent::ToolResult& result)
{
    auto code = result.errorCode.toLower();
    code.remove(QLatin1Char('_'));
    code.remove(QLatin1Char('-'));
    return code == QLatin1String("invalidargument") ||
           code == QLatin1String("invalidfield") ||
           code == QLatin1String("invalidfieldvalue") ||
           code == QLatin1String("invalidselector") ||
           code == QLatin1String("invalidtype") ||
           code == QLatin1String("unknownfield");
}

QJsonValue toolResultPayload(const agent::ToolResult& result)
{
    if (!result.structuredContent.isUndefined() &&
        !result.structuredContent.isNull())
        return result.structuredContent;
    const auto structured =
        result.result.value(QStringLiteral("structuredContent"));
    return structured.isUndefined() || structured.isNull()
               ? QJsonValue(result.result)
               : structured;
}

QJsonObject selectedResultObject(const agent::ToolResult& result)
{
    auto payload = toolResultPayload(result);
    if (!payload.isObject()) return {};
    auto object = payload.toObject();
    if (object.value(QStringLiteral("result")).isObject())
        object = object.value(QStringLiteral("result")).toObject();
    return object;
}

QString objectIdentityMismatch(const agent::Action& action,
                               const agent::ToolResult& result)
{
    if (unqualifiedToolName(action.toolName) != QLatin1String("get_object"))
        return {};
    const auto selector = actionSelector(action);
    if (selector.isEmpty()) return {};
    const auto object = selectedResultObject(result);
    const auto returnedName =
        object.value(QStringLiteral("name")).toString().trimmed();
    const auto returnedUuid = stringArgument(
        object, {QStringLiteral("uuid"), QStringLiteral("object_uuid")});
    const auto foldedSelector = selector.toCaseFolded();
    if (returnedName.compare(QStringLiteral("template"), Qt::CaseInsensitive) ==
            0 &&
        !foldedSelector.contains(QStringLiteral("template")))
        return QStringLiteral(
                   "The tool returned a default template instead of the "
                   "requested object selector %1.")
            .arg(selector);
    if (!returnedName.isEmpty() && !returnedUuid.isEmpty() &&
        !foldedSelector.contains(returnedName.toCaseFolded()) &&
        !foldedSelector.contains(returnedUuid.toCaseFolded()))
        return QStringLiteral(
                   "The returned object identity (%1, %2) does not match "
                   "the requested selector %3.")
            .arg(returnedName, returnedUuid, selector);
    return {};
}

QString findStringField(const QJsonValue& value, const QString& field,
                        qsizetype depth = 0)
{
    if (depth > 8) return {};
    if (value.isArray())
    {
        for (const auto& item : value.toArray())
        {
            const auto found = findStringField(item, field, depth + 1);
            if (!found.isEmpty()) return found;
        }
        return {};
    }
    if (!value.isObject()) return {};
    const auto object = value.toObject();
    const auto direct = object.value(field);
    if (direct.isString() && !direct.toString().trimmed().isEmpty())
        return direct.toString().trimmed();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        const auto found = findStringField(item.value(), field, depth + 1);
        if (!found.isEmpty()) return found;
    }
    return {};
}

QString normalizedScope(QString value)
{
    value = value.trimmed();
    value.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (value.endsWith(QLatin1Char('/')))
        value.chop(1);
    return value.toCaseFolded();
}

QString requestedContextScope(const agent::Action& action)
{
    auto scope =
        stringArgument(action.arguments, {QStringLiteral("case_path")});
    if (scope.isEmpty())
    {
        auto parent = stringArgument(action.arguments,
                                     {QStringLiteral("parent_directory")});
        const auto name =
            stringArgument(action.arguments, {QStringLiteral("case_name")});
        if (!parent.isEmpty() && !name.isEmpty())
        {
            while (parent.endsWith(QLatin1Char('/')) ||
                   parent.endsWith(QLatin1Char('\\')))
                parent.chop(1);
            scope = parent + QLatin1Char('/') + name;
        }
    }
    return normalizedScope(scope);
}

QString contextScope(const agent::Action& action,
                     const agent::ToolResult& result)
{
    auto scope =
        findStringField(toolResultPayload(result), QStringLiteral("case_path"));
    if (scope.isEmpty()) return requestedContextScope(action);
    return normalizedScope(scope);
}

QString findReferencedId(const QJsonValue& value,
                         const QStringList& invalidatedIds, qsizetype depth = 0)
{
    if (depth > 12) return {};
    if (value.isString())
    {
        const auto text = value.toString().toCaseFolded();
        for (const auto& id : invalidatedIds)
            if (!id.isEmpty() && text.contains(id.toCaseFolded())) return id;
        return {};
    }
    if (value.isArray())
    {
        for (const auto& item : value.toArray())
        {
            const auto found =
                findReferencedId(item, invalidatedIds, depth + 1);
            if (!found.isEmpty()) return found;
        }
        return {};
    }
    if (!value.isObject()) return {};
    const auto object = value.toObject();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item)
    {
        const auto found =
            findReferencedId(item.value(), invalidatedIds, depth + 1);
        if (!found.isEmpty()) return found;
    }
    return {};
}

QJsonValue canonicalJsonValue(const QJsonValue& value)
{
    if (value.isString())
    {
        auto text = value.toString();
        if (text.size() >= 3 && text.at(1) == QLatin1Char(':') &&
            (text.at(2) == QLatin1Char('/') || text.at(2) == QLatin1Char('\\')))
            text.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return text;
    }
    if (value.isArray())
    {
        QJsonArray result;
        for (const auto& item : value.toArray())
            result.append(canonicalJsonValue(item));
        return result;
    }
    if (value.isObject())
    {
        QJsonObject result;
        const auto object = value.toObject();
        for (const auto& key : object.keys())
            result.insert(key, canonicalJsonValue(object.value(key)));
        return result;
    }
    return value;
}

QString toolCallSignature(const agent::Action& action)
{
    const auto arguments = canonicalJsonValue(action.arguments).toObject();
    return action.toolName + QLatin1Char('\n') +
           QString::fromUtf8(
               QJsonDocument(arguments).toJson(QJsonDocument::Compact));
}

QString repeatedCompletedCallError(const QStringList& history,
                                   const QString& candidate)
{
    auto sequence = history;
    sequence.append(candidate);
    for (qsizetype cycleLength = 1; cycleLength <= maximumDetectedCycleLength &&
                                    sequence.size() >= cycleLength * 2;
         ++cycleLength)
    {
        const auto cycleStart = sequence.size() - cycleLength * 2;
        auto repeats = true;
        for (qsizetype offset = 0; offset < cycleLength; ++offset)
        {
            if (sequence.at(cycleStart + offset) !=
                sequence.at(cycleStart + cycleLength + offset))
            {
                repeats = false;
                break;
            }
        }
        if (!repeats) continue;
        if (cycleLength == 1)
            return QStringLiteral(
                "The identical tool call already completed successfully. "
                "Do not execute it again. Continue with a different "
                "unfinished step, or return final if all requested work is "
                "complete.");
        return QStringLiteral(
            "This tool call would continue a repeated cycle of completed "
            "calls. Do not toggle resources open and closed or repeat "
            "completed work. Continue with a different unfinished step, or "
            "return final if all requested work is complete.");
    }
    return {};
}

QString eventDataSummary(const QJsonObject& data)
{
    auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (bytes.size() > maximumLoggedEventDataBytes)
    {
        bytes = bytes.left(maximumLoggedEventDataBytes);
        bytes.append("...");
    }
    return QString::fromUtf8(bytes);
}

QString singleLine(QString value)
{
    return value.replace(QLatin1Char('\r'), QLatin1Char(' '))
        .replace(QLatin1Char('\n'), QLatin1Char(' '))
        .trimmed();
}

QString unfinishedEvidenceReason(const QList<QJsonObject>& toolEvidence,
                                 const AgentLedger& ledger)
{
    if (!toolEvidence.isEmpty())
    {
        const auto& latest = toolEvidence.constLast();
        if (latest.value(QStringLiteral("outcome")).toString() ==
            QLatin1String("error"))
            return QStringLiteral(
                "The most recent tool call failed, so the requested operation "
                "does not yet have successful evidence.");
        if (!latest.value(QStringLiteral("terminal")).toBool())
            return QStringLiteral(
                "The most recent tool result is still running or pending and "
                "is not terminal evidence.");
    }
    return ledger.unresolvedVerificationReason();
}

ToolOperationKind operationKind(
    const agent::ToolDefinition& tool,
    const AgentController::ToolRiskHandler& toolRisk)
{
    const auto readOnlyHint =
        tool.annotations.value(QStringLiteral("readOnlyHint"));
    if (readOnlyHint.isBool())
        return readOnlyHint.toBool() ? ToolOperationKind::ReadOnly
                                     : ToolOperationKind::Mutation;
    if (tool.annotations.value(QStringLiteral("destructiveHint")).toBool())
        return ToolOperationKind::Mutation;
    if (!toolRisk) return ToolOperationKind::Unknown;
    switch (toolRisk(tool.qualifiedName))
    {
        case infrastructure::mcp::ToolRisk::ReadOnly:
            return ToolOperationKind::ReadOnly;
        case infrastructure::mcp::ToolRisk::CreatesData:
        case infrastructure::mcp::ToolRisk::Destructive:
            return ToolOperationKind::Mutation;
        case infrastructure::mcp::ToolRisk::ModifiesData:
            return ToolOperationKind::Unknown;
    }
    return ToolOperationKind::Unknown;
}

bool isDiscoveryToolName(const QString& qualifiedToolName)
{
    const auto name = qualifiedToolName.section(QLatin1Char('.'), -1).toLower();
    static const QStringList verbs{
        QStringLiteral("list"),     QStringLiteral("get"),
        QStringLiteral("describe"), QStringLiteral("inspect"),
        QStringLiteral("find"),     QStringLiteral("query"),
        QStringLiteral("search"),   QStringLiteral("read"),
        QStringLiteral("fetch"),    QStringLiteral("lookup"),
        QStringLiteral("retrieve"), QStringLiteral("enumerate"),
        QStringLiteral("scan"),     QStringLiteral("view"),
        QStringLiteral("snapshot")};
    static const QRegularExpression separator(QStringLiteral("[^a-z0-9]+"));
    const auto tokens = name.split(separator, Qt::SkipEmptyParts);
    return std::any_of(tokens.cbegin(), tokens.cend(), [](const QString& token)
                       { return verbs.contains(token); });
}

qsizetype messageCharacters(const QList<chat::Message>& messages)
{
    qsizetype total = 0;
    for (const auto& message : messages)
        total += message.content.size();
    return total;
}
}  // namespace

AgentController::AgentController(Dependencies dependencies, QObject* parent)
    : QObject(parent),
      dependencies_(std::move(dependencies)),
      runTimer_(new QTimer(this)),
      pollTimer_(new QTimer(this))
{
    qRegisterMetaType<AgentProgressSnapshot>();
    runTimer_->setSingleShot(true);
    runTimer_->setInterval(runTimeoutMilliseconds);
    connect(runTimer_, &QTimer::timeout, this,
            &AgentController::notifyLongRunning);
    pollTimer_->setSingleShot(true);
    connect(pollTimer_, &QTimer::timeout, this,
            &AgentController::executePendingPoll);
}

AgentController::~AgentController() = default;

void AgentController::notifyLongRunning()
{
    if (!hasActiveRun()) return;
    recordEvent(
        agent::EventType::Warning,
        QStringLiteral("Agent is still running after two minutes. It will "
                       "continue until completion or Stop."));
}

bool AgentController::start(const QString& userRequest,
                            const models::InferencePreset& preset,
                            const QList<agent::ToolDefinition>& tools,
                            const AssistantContext& context)
{
    const auto request = userRequest.trimmed();
    if (request.isEmpty() || hasActiveRun() || !dependencies_.generate ||
        !dependencies_.callTool || !dependencies_.validateTool ||
        !dependencies_.toolPolicy)
        return false;

    AgentRun run;
    run.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    run.userRequest = request;
    run.startedAt = QDateTime::currentDateTimeUtc();
    run.inferenceMessages = AgentPromptBuilder::initialMessages(
        request, tools, conversationMessages_, context);
    run.requestMessageIndex = run.inferenceMessages.size() - 1;
    run.taskPlanRequired =
        !tools.isEmpty() &&
        AgentPromptBuilder::requiresCompletionReview(request);
    if (run.taskPlanRequired)
    {
        // Keep the initial prompt as one user turn; the worker requires
        // strictly alternating user and assistant messages.
        run.inferenceMessages.last().content +=
            QStringLiteral("\n\n") +
            AgentPromptBuilder::taskPlanMessage(request).content;
    }
    activeRun_ = std::move(run);
    preset_ = preset;
    availableTools_ = tools;
    pendingApproval_.reset();
    activeToolAction_.reset();
    pendingPollAction_.reset();
    decisionBytes_.clear();
    activeToolCallSignature_.clear();
    lastFailedToolCallSignature_.clear();
    pollableToolCallSignature_.clear();
    lastPollCompletedAtMs_ = 0;
    completedToolCallHistory_.clear();
    toolEvidence_.clear();
    contractRecoveries_.clear();
    contractFailureCounts_.clear();
    modelContractPaths_.clear();
    activeContextScope_.clear();
    invalidatedResourceIds_.clear();
    contextEstablished_ = false;
    hasStateChangesInContext_ = false;
    if (pollTimer_->isActive()) pollTimer_->stop();
    if (!runTimer_->isActive()) runTimer_->start();

    recordEvent(agent::EventType::RunStarted,
                QStringLiteral("Agent run started."));
    emit userRequestAccepted(activeRun_->id, request);
    requestDecision();
    return true;
}

void AgentController::cancel()
{
    if (!hasActiveRun()) return;
    const auto previousState = state_;
    const auto toolRequestId = activeRun_->toolRequestId;
    activeRun_->finishCode = QStringLiteral("cancelled");
    activeRun_->finishMessage = QStringLiteral("Agent run cancelled.");
    setState(AgentRun::State::Cancelled);
    if (runTimer_->isActive()) runTimer_->stop();
    if (pollTimer_->isActive()) pollTimer_->stop();
    pendingApproval_.reset();
    activeToolAction_.reset();
    pendingPollAction_.reset();
    pollableToolCallSignature_.clear();
    lastPollCompletedAtMs_ = 0;
    activeRun_->toolRequestId.clear();
    if (previousState == AgentRun::State::Deciding &&
        dependencies_.cancelGeneration)
        dependencies_.cancelGeneration();
    if (previousState == AgentRun::State::ExecutingTool &&
        !toolRequestId.isEmpty() && dependencies_.cancelTool)
        dependencies_.cancelTool(toolRequestId);
    recordEvent(agent::EventType::Cancelled,
                QStringLiteral("Agent run cancelled."));
    emit metricsReady(activeRun_->id,
                      AgentRunMetrics::fromRun(*activeRun_).toJson());
    emit runFinished(activeRun_->id, state_, QStringLiteral("cancelled"),
                     QStringLiteral("Agent run cancelled."));
}

void AgentController::resolveApproval(bool approved)
{
    if (!hasActiveRun() || state_ != AgentRun::State::WaitingForApproval ||
        !pendingApproval_.has_value())
        return;
    const auto action = *pendingApproval_;
    pendingApproval_.reset();
    if (!approved)
    {
        failRun(QStringLiteral("approval_denied"),
                QStringLiteral("The requested tool call was rejected."));
        return;
    }
    executeTool(action);
}

bool AgentController::clearConversation()
{
    if (hasActiveRun()) return false;
    conversationMessages_.clear();
    emit conversationCleared();
    return true;
}

bool AgentController::setConversationMessages(QList<chat::Message> messages)
{
    if (hasActiveRun()) return false;
    conversationMessages_ = std::move(messages);
    return true;
}

AgentRun::State AgentController::state() const
{
    return state_;
}

bool AgentController::hasActiveRun() const
{
    return activeRun_.has_value() && !isTerminal(state_);
}

const std::optional<AgentRun>& AgentController::activeRun() const
{
    return activeRun_;
}

const QList<chat::Message>& AgentController::conversationMessages() const
{
    return conversationMessages_;
}

bool AgentController::hasConversation() const
{
    return !conversationMessages_.isEmpty();
}

AgentProgressSnapshot AgentController::progressSnapshot() const
{
    AgentProgressSnapshot snapshot;
    if (!activeRun_) return snapshot;

    const auto& run = *activeRun_;
    snapshot.runId = run.id;
    snapshot.state = run.state;
    snapshot.elapsedMilliseconds =
        qMax<qint64>(0, run.startedAt.msecsTo(QDateTime::currentDateTimeUtc()));
    snapshot.completedToolCount = run.successfulToolResults;
    snapshot.evidenceCount = static_cast<int>(toolEvidence_.size());
    snapshot.finishCode = run.finishCode;
    snapshot.finishMessage = run.finishMessage;

    const auto terminal = isTerminal(run.state);
    auto assignedCurrentStep = false;
    for (const auto& value : run.completionSteps)
    {
        const auto object = value.toObject();
        AgentProgressStep step;
        step.id = object.value(QStringLiteral("id")).toString();
        step.description =
            object.value(QStringLiteral("description")).toString();
        const auto status =
            object.value(QStringLiteral("status")).toString().toLower();
        if (status == QLatin1String("satisfied"))
            step.status = AgentProgressStepStatus::Completed;
        else if (status == QLatin1String("blocked"))
            step.status = AgentProgressStepStatus::Blocked;
        else if (run.state == AgentRun::State::Completed && status.isEmpty())
            step.status = AgentProgressStepStatus::Completed;
        else if (!terminal && !assignedCurrentStep)
        {
            step.status = AgentProgressStepStatus::Current;
            snapshot.currentStepId = step.id;
            assignedCurrentStep = true;
        }
        snapshot.steps.append(std::move(step));
    }

    const auto toolOperation = [](const std::optional<agent::Action>& action)
    {
        return action.has_value()
                   ? QStringLiteral("Calling %1").arg(action->toolName)
                   : QString{};
    };
    switch (run.state)
    {
        case AgentRun::State::Idle:
            snapshot.operation = QStringLiteral("Starting agent run");
            snapshot.waitingReason = QStringLiteral("Preparing the task");
            break;
        case AgentRun::State::Deciding:
            snapshot.operation = QStringLiteral("Choosing the next action");
            snapshot.waitingReason =
                QStringLiteral("Waiting for the model response");
            break;
        case AgentRun::State::WaitingForApproval:
            snapshot.operation = toolOperation(pendingApproval_);
            snapshot.waitingReason =
                QStringLiteral("Waiting for your approval");
            break;
        case AgentRun::State::ExecutingTool:
            snapshot.operation = toolOperation(activeToolAction_);
            if (snapshot.operation.isEmpty())
                snapshot.operation = toolOperation(pendingPollAction_);
            snapshot.waitingReason =
                pendingPollAction_.has_value()
                    ? QStringLiteral("Waiting for the next status check")
                    : QStringLiteral("Waiting for the tool result");
            break;
        case AgentRun::State::GeneratingAnswer:
            snapshot.operation = QStringLiteral("Preparing the final answer");
            break;
        case AgentRun::State::Completed:
            snapshot.operation = QStringLiteral("Agent run completed");
            break;
        case AgentRun::State::Cancelled:
            snapshot.operation = QStringLiteral("Agent run stopped");
            break;
        case AgentRun::State::Failed:
            snapshot.operation = QStringLiteral("Agent run failed");
            break;
    }

    const auto firstEvent =
        qMax<qsizetype>(0, run.events.size() - maximumProgressActivities);
    for (auto index = firstEvent; index < run.events.size(); ++index)
    {
        const auto& event = run.events.at(index);
        snapshot.recentActivity.append({event.type, event.message.left(240),
                                        event.toolName.left(160),
                                        event.timestamp});
    }
    return snapshot;
}

void AgentController::receiveToken(const QByteArray& bytes)
{
    if (!hasActiveRun() || state_ != AgentRun::State::Deciding ||
        bytes.isEmpty())
        return;
    decisionBytes_ += bytes;
    if (decisionBytes_.size() > maximumDecisionBytes)
    {
        failRun(QStringLiteral("decision_too_large"),
                QStringLiteral("Agent decision exceeded 64 KiB."));
    }
}

void AgentController::completeGeneration(bool cancelled,
                                         const QJsonObject& metrics)
{
    if (!hasActiveRun() || state_ != AgentRun::State::Deciding) return;
    const auto promptTokens =
        metrics.value(QStringLiteral("promptTokens")).toInt();
    if (promptTokens > 0) activeRun_->lastPromptTokens = promptTokens;
    if (cancelled)
    {
        failRun(QStringLiteral("generation_cancelled"),
                QStringLiteral("Agent decision generation was cancelled."));
        return;
    }

    agent::Action action;
    QString errorMessage;
    if (!agent::parseAction(decisionBytes_, action, errorMessage))
    {
        retryInvalidAction(decisionBytes_, errorMessage);
        return;
    }
    handleAction(action, decisionBytes_);
}

void AgentController::handleGenerationError(const QString& code,
                                            const QString& message)
{
    if (!hasActiveRun() || state_ != AgentRun::State::Deciding) return;
    failRun(code.isEmpty() ? QStringLiteral("generation_failed") : code,
            message);
}

void AgentController::receiveToolResult(const agent::ToolResult& result)
{
    if (!hasActiveRun() || state_ != AgentRun::State::ExecutingTool ||
        result.requestId != activeRun_->toolRequestId)
        return;
    auto normalizedResult = result;
    normalizedResult.outcome = normalizedToolOutcome(result);
    normalizedResult.sideEffectState = normalizedToolSideEffectState(result);
    normalizedResult.isError =
        normalizedResult.outcome != agent::ToolOutcome::Succeeded &&
        normalizedResult.outcome != agent::ToolOutcome::InProgress;
    activeRun_->toolRequestId.clear();
    const auto completedToolCallSignature =
        std::exchange(activeToolCallSignature_, QString{});
    const auto completedToolAction =
        std::exchange(activeToolAction_, std::nullopt);
    auto completedOperationKind = ToolOperationKind::Unknown;
    auto outputSchemaValidated = false;
    if (completedToolAction.has_value())
    {
        const auto definition = std::find_if(
            availableTools_.cbegin(), availableTools_.cend(),
            [&completedToolAction](const agent::ToolDefinition& candidate)
            {
                return candidate.qualifiedName == completedToolAction->toolName;
            });
        completedOperationKind =
            definition == availableTools_.cend()
                ? ToolOperationKind::Unknown
                : operationKind(*definition, dependencies_.toolRisk);
        outputSchemaValidated = definition != availableTools_.cend() &&
                                definition->hasOutputSchema &&
                                !definition->outputSchema.isEmpty();
    }

    QString recoveryGuidance;
    auto contractFailuresExhausted = false;
    if (completedToolAction.has_value() &&
        normalizedResult.outcome == agent::ToolOutcome::Succeeded)
    {
        const auto mismatch =
            objectIdentityMismatch(*completedToolAction, normalizedResult);
        if (!mismatch.isEmpty())
        {
            normalizedResult.isError = true;
            normalizedResult.failureKind = agent::ToolFailureKind::Tool;
            normalizedResult.outcome = agent::ToolOutcome::ToolFailed;
            normalizedResult.sideEffectState =
                agent::ToolSideEffectState::KnownFailed;
            normalizedResult.errorCode =
                QStringLiteral("object_identity_mismatch");
            normalizedResult.errorMessage = mismatch;
            recoveryGuidance = QStringLiteral(
                "Do not use this result as object evidence. Reuse the "
                "object identity returned by the creation or listing call; "
                "do not substitute a geometry-resource identifier.");
        }
    }
    if (completedToolAction.has_value() &&
        normalizedResult.outcome == agent::ToolOutcome::Succeeded)
    {
        const auto contextGuidance = updateContextAfterSuccess(
            *completedToolAction, normalizedResult, completedOperationKind);
        if (!contextGuidance.isEmpty())
            recoveryGuidance +=
                (recoveryGuidance.isEmpty() ? QString{} : QStringLiteral(" ")) +
                contextGuidance;
        captureModelContract(*completedToolAction, normalizedResult);
        resolveContractRecovery(*completedToolAction);
    }
    if (completedToolAction.has_value() && isContractFailure(normalizedResult))
    {
        const auto contractGuidance = registerContractFailure(
            *completedToolAction, normalizedResult, contractFailuresExhausted);
        if (!contractGuidance.isEmpty())
            recoveryGuidance +=
                (recoveryGuidance.isEmpty() ? QString{} : QStringLiteral(" ")) +
                contractGuidance;
    }

    completedToolCallHistory_.append(completedToolCallSignature);
    const auto outcome = normalizedResult.outcome;
    const auto inProgress = outcome == agent::ToolOutcome::InProgress;
    if (completedToolAction.has_value() && !inProgress)
    {
        if (completedOperationKind == ToolOperationKind::ReadOnly &&
            isDiscoveryToolName(completedToolAction->toolName))
            ++activeRun_->consecutiveDiscoveryCalls;
        else
            activeRun_->consecutiveDiscoveryCalls = 0;
    }
    if (completedToolAction.has_value() &&
        outcome == agent::ToolOutcome::Succeeded &&
        isSingleContextOperationRequest(activeRun_->userRequest,
                                        completedToolAction->toolName))
    {
        activeRun_->completedSingleOperationTool =
            completedToolAction->toolName;
        recoveryGuidance +=
            (recoveryGuidance.isEmpty() ? QString{} : QStringLiteral(" ")) +
            QStringLiteral(
                "The sole operation requested by the user succeeded. "
                "Return final next. Do not call another tool, inspect the "
                "opened context, or perform cleanup.");
    }
    if (inProgress)
    {
        pollableToolCallSignature_ = completedToolCallSignature;
        lastPollCompletedAtMs_ = QDateTime::currentMSecsSinceEpoch();
    }
    else
    {
        pollableToolCallSignature_.clear();
        lastPollCompletedAtMs_ = 0;
    }
    auto evidenceSequence = 0;
    if (completedToolAction.has_value())
    {
        evidenceSequence = static_cast<int>(toolEvidence_.size() + 1);
        toolEvidence_.append(AgentContextCompactor::toolEvidence(
            evidenceSequence, *completedToolAction, normalizedResult));
        activeRun_->ledger.recordToolResult(
            evidenceSequence, *completedToolAction, normalizedResult,
            completedOperationKind, outputSchemaValidated);
        ++activeRun_->evidenceRevision;
        activeRun_->completionReviewsAtRevision = 0;
        activeRun_->completionReviewFailures = 0;
        activeRun_->completionPlanDriftRepairs = 0;
    }
    if (activeRun_->orderedTaskPlan && completedToolAction.has_value() &&
        outcome == agent::ToolOutcome::Succeeded && !inProgress &&
        evidenceSequence > 0 &&
        completedToolAction->completesPlanStep.value_or(false))
    {
        const auto planGuidance =
            advanceTaskPlanAfterToolResult(evidenceSequence);
        if (!planGuidance.isEmpty())
            recoveryGuidance +=
                (recoveryGuidance.isEmpty() ? QString{} : QStringLiteral(" ")) +
                planGuidance;
    }
    else if (activeRun_->orderedTaskPlan && completedToolAction.has_value() &&
             !inProgress && !completedToolAction->planStepId.isEmpty())
    {
        recoveryGuidance +=
            (recoveryGuidance.isEmpty() ? QString{} : QStringLiteral(" ")) +
            QStringLiteral(
                "Task-plan step '%1' remains current. Use the same "
                "plan_step_id for the next necessary call; do not advance to "
                "a later step yet.")
                .arg(completedToolAction->planStepId);
    }
    if (outcome != agent::ToolOutcome::Succeeded && !inProgress)
        lastFailedToolCallSignature_ = completedToolCallSignature;
    else
    {
        lastFailedToolCallSignature_.clear();
        if (outcome == agent::ToolOutcome::Succeeded)
            ++activeRun_->successfulToolResults;
    }
    activeRun_->stagnationRecoveries = 0;
    recordEvent(agent::EventType::ToolFinished,
                outcome == agent::ToolOutcome::Succeeded
                    ? QStringLiteral("Tool call completed.")
                : inProgress ? QStringLiteral("Tool call is still in progress.")
                             : normalizedResult.errorMessage,
                normalizedResult.serverId + QLatin1Char('.') +
                    normalizedResult.toolName,
                normalizedResult.result);
    activeRun_->inferenceMessages.append(AgentPromptBuilder::toolResultMessage(
        normalizedResult, evidenceSequence, activeRun_->ledger.snapshot(),
        activeRun_->ledger.unresolvedVerificationReason(), recoveryGuidance));

    const auto uncertainDispatch = normalizedResult.sideEffectState ==
                                   agent::ToolSideEffectState::Uncertain;
    const auto remoteFailure = outcome == agent::ToolOutcome::TransportFailed ||
                               outcome == agent::ToolOutcome::ProtocolFailed ||
                               outcome == agent::ToolOutcome::ServerFailed ||
                               outcome == agent::ToolOutcome::Cancelled;
    if (remoteFailure && uncertainDispatch &&
        completedOperationKind != ToolOperationKind::ReadOnly)
    {
        failRun(QStringLiteral("tool_side_effect_uncertain"),
                QStringLiteral(
                    "The tool request was dispatched, but its final outcome "
                    "is unknown. Verify the affected state before issuing "
                    "another mutating call."));
        return;
    }
    if (outcome == agent::ToolOutcome::Cancelled)
    {
        failRun(QStringLiteral("tool_cancelled"),
                normalizedResult.errorMessage.isEmpty()
                    ? QStringLiteral("The tool request was cancelled.")
                    : normalizedResult.errorMessage);
        return;
    }
    if (outcome == agent::ToolOutcome::TransportFailed &&
        (normalizedResult.sideEffectState ==
             agent::ToolSideEffectState::NotDispatched ||
         completedOperationKind == ToolOperationKind::ReadOnly) &&
        completedOperationKind == ToolOperationKind::ReadOnly &&
        activeRun_->readOnlyTransportRetries < 1 && completedToolAction)
    {
        ++activeRun_->readOnlyTransportRetries;
        const QJsonObject retryAction{
            {QStringLiteral("action"), QStringLiteral("call_tool")},
            {QStringLiteral("tool"), completedToolAction->toolName},
            {QStringLiteral("arguments"), completedToolAction->arguments}};
        activeRun_->inferenceMessages.append(
            {chat::Role::Assistant,
             QString::fromUtf8(
                 QJsonDocument(retryAction).toJson(QJsonDocument::Compact))});
        executeTool(*completedToolAction);
        return;
    }
    if (outcome == agent::ToolOutcome::Denied)
    {
        failRun(QStringLiteral("tool_denied"),
                normalizedResult.errorMessage.isEmpty()
                    ? QStringLiteral("The tool request was denied.")
                    : normalizedResult.errorMessage);
        return;
    }
    if (contractFailuresExhausted)
    {
        failRun(QStringLiteral("invalid_tool_arguments"),
                QStringLiteral(
                    "The same tool and target rejected corrected arguments %1 "
                    "times. Stop guessing field paths and report the contract "
                    "blocker.")
                    .arg(maximumContractFailuresPerTarget));
        return;
    }
    requestDecision();
}

bool AgentController::isTerminal(AgentRun::State state)
{
    return state == AgentRun::State::Completed ||
           state == AgentRun::State::Cancelled ||
           state == AgentRun::State::Failed;
}

void AgentController::requestDecision()
{
    if (!activeRun_) return;
    ++activeRun_->decisionCount;
    compactContextIfNeeded();
    decisionBytes_.clear();
    setState(AgentRun::State::Deciding);
    recordEvent(agent::EventType::DecisionStarted,
                QStringLiteral("Generating the next agent decision."));
    activeRun_->lastSubmittedCharacters =
        messageCharacters(activeRun_->inferenceMessages);
    dependencies_.generate(
        activeRun_->inferenceMessages, preset_,
        qMax(minimumDecisionTokens, preset_.maxOutputTokens));
}

void AgentController::compactContextIfNeeded()
{
    if (!activeRun_) return;
    auto estimatedPromptTokens = activeRun_->lastPromptTokens;
    const auto currentCharacters =
        messageCharacters(activeRun_->inferenceMessages);
    if (estimatedPromptTokens > 0 && activeRun_->lastSubmittedCharacters > 0 &&
        currentCharacters > activeRun_->lastSubmittedCharacters)
    {
        estimatedPromptTokens = static_cast<int>(
            (static_cast<qint64>(estimatedPromptTokens) * currentCharacters +
             activeRun_->lastSubmittedCharacters - 1) /
            activeRun_->lastSubmittedCharacters);
    }
    if (!AgentContextCompactor::shouldCompact(estimatedPromptTokens, preset_))
        return;
    const QJsonObject completionState{
        {QStringLiteral("steps"), activeRun_->completionSteps},
        {QStringLiteral("ordered"), activeRun_->orderedTaskPlan},
        {QStringLiteral("currentStepEvidenceStart"),
         activeRun_->currentPlanStepEvidenceStart},
        {QStringLiteral("evidenceRevision"), activeRun_->evidenceRevision},
        {QStringLiteral("lastReviewedEvidenceRevision"),
         activeRun_->lastReviewedEvidenceRevision},
        {QStringLiteral("awaitingReview"),
         activeRun_->awaitingCompletionReview}};
    const auto result = AgentContextCompactor::compact(
        activeRun_->inferenceMessages, activeRun_->requestMessageIndex,
        activeRun_->userRequest, toolEvidence_, completionState,
        activeRun_->ledger.snapshot(), preset_);
    if (!result.compacted) return;
    ++activeRun_->contextCompactions;
    qInfo().noquote()
        << QStringLiteral(
               "Agent context compacted: run=%1 promptTokens=%2 "
               "estimatedPromptTokens=%3 messages=%4->%5 evidence=%6 "
               "compactions=%7")
               .arg(activeRun_->id)
               .arg(activeRun_->lastPromptTokens)
               .arg(estimatedPromptTokens)
               .arg(result.messagesBefore)
               .arg(result.messagesAfter)
               .arg(toolEvidence_.size())
               .arg(activeRun_->contextCompactions);
    activeRun_->lastPromptTokens = 0;
}

void AgentController::handleAction(const agent::Action& action,
                                   const QByteArray& rawAction)
{
    if (!activeRun_) return;

    if (activeRun_->taskPlanRequired)
    {
        if (action.type == agent::ActionType::TaskPlan)
            acceptTaskPlan(action, rawAction);
        else
            retryTaskPlan(
                rawAction,
                QStringLiteral(
                    "A task_plan is required before executing or completing "
                    "this multi-step request."));
        return;
    }

    if (action.type == agent::ActionType::TaskPlan)
    {
        retryInvalidAction(
            rawAction,
            QStringLiteral(
                "task_plan is valid only when the controller requests it."));
        return;
    }

    if (activeRun_->awaitingCompletionReview)
    {
        if (action.type == agent::ActionType::ReviewCompletion)
        {
            handleCompletionReview(action, rawAction);
            return;
        }
        if (action.type == agent::ActionType::Final)
        {
            retryCompletionReview(
                rawAction,
                QStringLiteral(
                    "A second final does not verify completion. Return "
                    "review_completion with per-step evidence."));
            return;
        }

        activeRun_->awaitingCompletionReview = false;
        activeRun_->pendingFinalCandidate.clear();
        activeRun_->completionReviewFailures = 0;
    }
    else if (action.type == agent::ActionType::ReviewCompletion)
    {
        retryInvalidAction(
            rawAction,
            QStringLiteral(
                "review_completion is valid only after the controller asks "
                "to review a proposed final answer."));
        return;
    }

    if (action.type == agent::ActionType::Final)
    {
        if (activeRun_->orderedTaskPlan)
        {
            QString unfinishedToolStep;
            for (const auto& value : activeRun_->completionSteps)
            {
                const auto step = value.toObject();
                if (step.value(QStringLiteral("status")).toString() !=
                        QLatin1String("satisfied") &&
                    step.value(QStringLiteral("requires_tool")).toBool())
                {
                    unfinishedToolStep =
                        step.value(QStringLiteral("description")).toString();
                    break;
                }
            }
            if (!unfinishedToolStep.isEmpty())
            {
                retryUnfinishedFinal(
                    rawAction,
                    QStringLiteral(
                        "Ordered task-plan step '%1' has not been verified. "
                        "Complete the current step before returning final.")
                        .arg(unfinishedToolStep));
                return;
            }
            const auto verificationReason =
                activeRun_->ledger.unresolvedVerificationReason();
            if (!verificationReason.isEmpty())
            {
                retryUnfinishedFinal(rawAction, verificationReason);
                return;
            }
            for (qsizetype index = 0;
                 index < activeRun_->completionSteps.size(); ++index)
            {
                auto step = activeRun_->completionSteps.at(index).toObject();
                if (step.value(QStringLiteral("status")).toString() ==
                    QLatin1String("satisfied"))
                    continue;
                step.insert(QStringLiteral("status"),
                            QStringLiteral("satisfied"));
                step.insert(QStringLiteral("evidence"), QJsonArray{});
                activeRun_->completionSteps.replace(index, step);
            }
            completeRun(action.content);
        }
        else if (!activeRun_->completionSteps.isEmpty())
            beginCompletionReview(action, rawAction);
        else
        {
            const auto unfinishedReason =
                activeRun_->completedSingleOperationTool.isEmpty()
                    ? unfinishedEvidenceReason(toolEvidence_,
                                               activeRun_->ledger)
                    : QString{};
            if (unfinishedReason.isEmpty())
                completeRun(action.content);
            else
                retryUnfinishedFinal(rawAction, unfinishedReason);
        }
        return;
    }

    const auto nextPlanStep = [this]() -> QJsonObject
    {
        if (!activeRun_) return {};
        for (const auto& value : activeRun_->completionSteps)
        {
            const auto step = value.toObject();
            const auto status = step.value(QStringLiteral("status")).toString();
            if (status != QLatin1String("satisfied") &&
                status != QLatin1String("blocked"))
                return step;
        }
        return {};
    };

    ++activeRun_->toolActionAttempts;
    const auto recordDuplicateToolAction = [this, &action]
    {
        if (!activeRun_) return;
        ++activeRun_->duplicateToolActions;
        const auto definition = std::find_if(
            availableTools_.cbegin(), availableTools_.cend(),
            [&action](const agent::ToolDefinition& candidate)
            { return candidate.qualifiedName == action.toolName; });
        if (definition == availableTools_.cend()) return;
        const auto kind = operationKind(*definition, dependencies_.toolRisk);
        if (kind == ToolOperationKind::Mutation)
            ++activeRun_->duplicateMutationActions;
        if (kind == ToolOperationKind::ReadOnly &&
            isDiscoveryToolName(action.toolName))
            ++activeRun_->redundantDiscoveryCalls;
    };

    if (activeRun_->orderedTaskPlan && !activeRun_->completionSteps.isEmpty())
    {
        const auto step = nextPlanStep();
        if (step.isEmpty())
        {
            recordDuplicateToolAction();
            retryNoProgressAction(
                rawAction,
                QStringLiteral(
                    "All task-plan steps are already verified. Do not execute "
                    "another tool; return final."));
            return;
        }
        const auto planStepId = step.value(QStringLiteral("id")).toString();
        if (action.planStepId.isEmpty() ||
            !action.completesPlanStep.has_value())
        {
            recordDuplicateToolAction();
            retryNoProgressAction(
                rawAction,
                QStringLiteral(
                    "An ordered task plan is active. This call must include "
                    "plan_step_id='%1' and boolean completes_plan_step. Use "
                    "false for an intermediate call and true only for the "
                    "final call that should complete the current step.")
                    .arg(planStepId));
            return;
        }
        if (action.planStepId != planStepId)
        {
            recordDuplicateToolAction();
            retryNoProgressAction(
                rawAction,
                QStringLiteral(
                    "This call targets plan_step_id '%1', but the current "
                    "unfinished step is '%2'. Execute checklist steps in "
                    "order and use the current step id.")
                    .arg(action.planStepId, planStepId));
            return;
        }
        if (!step.value(QStringLiteral("requires_tool")).toBool())
        {
            recordDuplicateToolAction();
            retryNoProgressAction(
                rawAction,
                QStringLiteral(
                    "The current task-plan step '%1' does not require a tool. "
                    "Complete or report that step first; do not execute a "
                    "later tool step out of order.")
                    .arg(step.value(QStringLiteral("description")).toString()));
            return;
        }
    }

    const auto staleId = staleResourceReference(action.arguments);
    if (!staleId.isEmpty())
    {
        recordDuplicateToolAction();
        retryNoProgressAction(
            rawAction,
            QStringLiteral(
                "The arguments reuse identifier %1 from an external context "
                "that was closed or replaced. Use identifiers returned in "
                "the current context only.")
                .arg(staleId));
        return;
    }

    const auto recoveryError = contractRecoveryError(action);
    if (!recoveryError.isEmpty())
    {
        recordDuplicateToolAction();
        retryNoProgressAction(rawAction, recoveryError);
        return;
    }

    const auto modelPathError = modelContractError(action);
    if (!modelPathError.isEmpty())
    {
        recordDuplicateToolAction();
        retryNoProgressAction(rawAction, modelPathError);
        return;
    }

    if (isSameActiveContextOperation(action))
    {
        recordDuplicateToolAction();
        retryNoProgressAction(
            rawAction,
            QStringLiteral(
                "This case is already open. Do not call the case-opening "
                "tool again and do not ask for approval; continue with the "
                "next requested operation."));
        return;
    }

    const auto signature = toolCallSignature(action);
    if (!lastFailedToolCallSignature_.isEmpty() &&
        signature == lastFailedToolCallSignature_)
    {
        recordDuplicateToolAction();
        retryNoProgressAction(
            rawAction,
            QStringLiteral(
                "The identical tool call already failed. Do not call it "
                "again. Return a final action now, or use meaningfully "
                "different arguments only when the user's request requires "
                "another attempt."));
        return;
    }

    const auto isStatusPoll = !pollableToolCallSignature_.isEmpty() &&
                              signature == pollableToolCallSignature_;
    const auto repeatedCallError =
        isStatusPoll
            ? QString{}
            : repeatedCompletedCallError(completedToolCallHistory_, signature);
    if (!repeatedCallError.isEmpty())
    {
        recordDuplicateToolAction();
        retryNoProgressAction(rawAction, repeatedCallError);
        return;
    }

    const auto tool =
        std::find_if(availableTools_.cbegin(), availableTools_.cend(),
                     [&action](const agent::ToolDefinition& candidate)
                     { return candidate.qualifiedName == action.toolName; });
    if (tool == availableTools_.cend())
    {
        retryInvalidAction(
            rawAction,
            QStringLiteral("Tool is not available: %1").arg(action.toolName));
        return;
    }
    ++activeRun_->toolValidationAttempts;
    const auto validation =
        dependencies_.validateTool(action.toolName, action.arguments);
    if (!validation.valid)
    {
        ++activeRun_->toolValidationFailures;
        auto issue = validation.issue;
        if (issue.toolName.isEmpty()) issue.toolName = action.toolName;
        if (issue.instancePath.isEmpty())
            issue.instancePath = QStringLiteral("arguments");
        if (issue.schemaPath.isEmpty()) issue.schemaPath = QStringLiteral("#");
        if (issue.keyword.isEmpty())
            issue.keyword = QStringLiteral("validation");
        if (issue.message.isEmpty())
            issue.message =
                QStringLiteral("Tool arguments failed local validation.");
        retryInvalidToolAction(rawAction, issue, *tool, action.arguments);
        return;
    }
    activeRun_->consecutiveValidationFailures = 0;
    if (!activeRun_->completedSingleOperationTool.isEmpty())
    {
        recordDuplicateToolAction();
        retryNoProgressAction(
            rawAction,
            QStringLiteral(
                "%1 already completed the user's sole requested operation. "
                "The next action must be final; no additional tool call is "
                "within scope.")
                .arg(activeRun_->completedSingleOperationTool));
        return;
    }
    if (closesContext(action.toolName) && contextEstablished_ &&
        !explicitlyRequestsContextClose(activeRun_->userRequest))
    {
        recordDuplicateToolAction();
        retryNoProgressAction(
            rawAction,
            QStringLiteral(
                "The user did not request closing the active case. Keep it "
                "open and return final when the requested work is complete; "
                "do not close it as inspection or cleanup."));
        return;
    }
    const auto candidateOperationKind =
        operationKind(*tool, dependencies_.toolRisk);
    const auto isBreadthDiscovery =
        candidateOperationKind == ToolOperationKind::ReadOnly &&
        isDiscoveryToolName(action.toolName);
    if (isBreadthDiscovery &&
        activeRun_->consecutiveDiscoveryCalls >=
            maximumConsecutiveDiscoveryCalls &&
        !explicitlyRequestsExhaustiveDiscovery(activeRun_->userRequest) &&
        !isRequiredContractDiscovery(action) && !isStatusPoll)
    {
        recordDuplicateToolAction();
        retryNoProgressAction(
            rawAction,
            QStringLiteral(
                "The run already made %1 consecutive read-only discovery "
                "calls without executing another requested operation. Do "
                "not broaden the inventory. Use existing evidence to return "
                "final, or call only a non-discovery tool required by an "
                "explicit unfinished outcome.")
                .arg(maximumConsecutiveDiscoveryCalls));
        return;
    }
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    const auto decision = dependencies_.toolPolicy(action.toolName);
    if (decision == infrastructure::mcp::ToolDecision::Deny)
    {
        failRun(QStringLiteral("tool_denied"),
                QStringLiteral("Local policy denied the tool call."));
        return;
    }
    if (decision == infrastructure::mcp::ToolDecision::RequireApproval ||
        requiresContextResetApproval(action))
    {
        pendingApproval_ = action;
        setState(AgentRun::State::WaitingForApproval);
        recordEvent(agent::EventType::ApprovalRequested,
                    QStringLiteral("Tool call requires approval."),
                    action.toolName, action.arguments);
        emit approvalRequested(activeRun_->id, action.toolName,
                               action.arguments);
        return;
    }
    executeTool(action);
}

void AgentController::acceptTaskPlan(const agent::Action& action,
                                     const QByteArray& rawAction)
{
    if (!activeRun_) return;
    if (!action.orderedPlan.has_value())
    {
        retryTaskPlan(
            rawAction,
            QStringLiteral(
                "task_plan must explicitly include ordered=true so the "
                "controller can enforce step-by-step execution."));
        return;
    }
    if (*action.orderedPlan)
    {
        auto sawNonToolStep = false;
        for (const auto& value : action.completionSteps)
        {
            const auto requiresTool =
                value.toObject()
                    .value(QStringLiteral("requires_tool"))
                    .toBool();
            if (!requiresTool)
                sawNonToolStep = true;
            else if (sawNonToolStep)
            {
                retryTaskPlan(
                    rawAction,
                    QStringLiteral(
                        "An ordered task plan cannot place a tool-required "
                        "step after a non-tool step. Keep all external "
                        "operations in user order and reserve non-tool steps "
                        "for the trailing final response."));
                return;
            }
        }
    }
    activeRun_->completionSteps = action.completionSteps;
    activeRun_->orderedTaskPlan = *action.orderedPlan;
    activeRun_->currentPlanStepEvidenceStart =
        static_cast<int>(toolEvidence_.size() + 1);
    activeRun_->taskPlanRequired = false;
    activeRun_->taskPlanFailures = 0;
    qInfo().noquote() << QStringLiteral(
                             "Agent task plan accepted: run=%1 steps=%2")
                             .arg(activeRun_->id)
                             .arg(activeRun_->completionSteps.size());
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::taskPlanAcceptedMessage(
            activeRun_->completionSteps));
    recordEvent(
        agent::EventType::TaskPlanAccepted,
        QStringLiteral("Task plan accepted."), {},
        {{QStringLiteral("stepCount"), activeRun_->completionSteps.size()}});
    requestDecision();
}

void AgentController::beginCompletionReview(const agent::Action& action,
                                            const QByteArray& rawAction)
{
    if (!activeRun_) return;
    ++activeRun_->completionReviewAttempts;
    if (activeRun_->lastReviewedEvidenceRevision ==
        activeRun_->evidenceRevision)
        ++activeRun_->completionReviewsAtRevision;
    else
    {
        activeRun_->lastReviewedEvidenceRevision = activeRun_->evidenceRevision;
        activeRun_->completionReviewsAtRevision = 1;
    }

    if (activeRun_->completionReviewsAtRevision >
        maximumCompletionReviewsPerEvidenceRevision)
    {
        failRun(QStringLiteral("completion_unverified"),
                QStringLiteral(
                    "The Agent proposed completion repeatedly without new tool "
                    "evidence after an unfinished completion review."));
        return;
    }

    activeRun_->pendingFinalCandidate = action.content;
    activeRun_->awaitingCompletionReview = true;
    activeRun_->completionReviewFailures = 0;
    qInfo().noquote()
        << QStringLiteral(
               "Agent completion review started: run=%1 evidenceRevision=%2 "
               "attemptAtRevision=%3 steps=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->evidenceRevision)
               .arg(activeRun_->completionReviewsAtRevision)
               .arg(activeRun_->completionSteps.size());
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::completionReviewMessage(
            activeRun_->userRequest, activeRun_->completionSteps, toolEvidence_,
            activeRun_->ledger.snapshot(),
            activeRun_->ledger.unresolvedVerificationReason()));
    requestDecision();
}

QString AgentController::validateCompletionReview(
    const agent::Action& action) const
{
    if (!activeRun_ || !activeRun_->awaitingCompletionReview ||
        activeRun_->pendingFinalCandidate.isEmpty())
        return QStringLiteral("No proposed final answer is awaiting review.");

    for (const auto& plannedValue : activeRun_->completionSteps)
    {
        const auto planned = plannedValue.toObject();
        const auto plannedId = planned.value(QStringLiteral("id")).toString();
        auto found = false;
        for (const auto& reviewedValue : action.completionSteps)
        {
            const auto reviewed = reviewedValue.toObject();
            if (reviewed.value(QStringLiteral("id")).toString() != plannedId)
                continue;
            found = true;
            if (reviewed.value(QStringLiteral("description")) !=
                    planned.value(QStringLiteral("description")) ||
                reviewed.value(QStringLiteral("requires_tool")) !=
                    planned.value(QStringLiteral("requires_tool")))
                return QStringLiteral(
                           "Completion step %1 changed its recorded "
                           "description or requires_tool value.")
                    .arg(plannedId);
            const auto recordedStatus =
                planned.value(QStringLiteral("status")).toString();
            const auto reviewedStatus =
                reviewed.value(QStringLiteral("status")).toString();
            if (recordedStatus == QLatin1String("satisfied") &&
                reviewedStatus != QLatin1String("satisfied"))
                return QStringLiteral(
                           "Completion step %1 was already verified by the "
                           "controller and cannot be moved back to %2.")
                    .arg(plannedId, reviewedStatus);
            break;
        }
        if (!found)
            return QStringLiteral("Completion review omitted recorded step %1.")
                .arg(plannedId);
    }

    auto priorStepUnfinished = false;
    for (const auto& reviewedValue : action.completionSteps)
    {
        const auto reviewed = reviewedValue.toObject();
        const auto id = reviewed.value(QStringLiteral("id")).toString();
        const auto status = reviewed.value(QStringLiteral("status")).toString();
        const auto requiresTool =
            reviewed.value(QStringLiteral("requires_tool")).toBool();
        const auto sequences =
            reviewed.value(QStringLiteral("evidence")).toArray();
        if (priorStepUnfinished && status == QLatin1String("satisfied"))
            return QStringLiteral(
                       "Completion step %1 cannot be satisfied before all "
                       "preceding task-plan steps are satisfied.")
                .arg(id);
        auto hasTerminalSuccess = false;
        auto citesUnverifiedMutation = false;
        for (const auto& sequenceValue : sequences)
        {
            const auto sequence = sequenceValue.toInt();
            auto found = false;
            for (const auto& evidence : toolEvidence_)
            {
                if (evidence.value(QStringLiteral("sequence")).toInt() !=
                    sequence)
                    continue;
                found = true;
                hasTerminalSuccess =
                    hasTerminalSuccess ||
                    (evidence.value(QStringLiteral("outcome")).toString() ==
                         QLatin1String("success") &&
                     evidence.value(QStringLiteral("terminal")).toBool());
                citesUnverifiedMutation =
                    citesUnverifiedMutation ||
                    activeRun_->ledger.evidenceRequiresVerification(sequence);
                break;
            }
            if (!found)
                return QStringLiteral(
                           "Completion step %1 cites unknown tool evidence "
                           "sequence %2.")
                    .arg(id)
                    .arg(sequence);
        }
        if (status == QLatin1String("satisfied") && requiresTool &&
            !hasTerminalSuccess)
            return QStringLiteral(
                       "Completion step %1 requires successful terminal tool "
                       "evidence.")
                .arg(id);
        if (status == QLatin1String("satisfied") && citesUnverifiedMutation)
            return QStringLiteral(
                       "Completion step %1 cites a mutation whose read-back "
                       "verification is still unresolved.")
                .arg(id);
        if (status != QLatin1String("satisfied")) priorStepUnfinished = true;
    }
    if (action.completionVerdict == QLatin1String("complete") &&
        activeRun_->ledger.hasUnresolvedVerification())
        return activeRun_->ledger.unresolvedVerificationReason();
    return {};
}

QString AgentController::advanceTaskPlanAfterToolResult(int evidenceSequence)
{
    if (!activeRun_ || activeRun_->completionSteps.isEmpty()) return {};

    for (qsizetype index = 0; index < activeRun_->completionSteps.size();
         ++index)
    {
        auto step = activeRun_->completionSteps.at(index).toObject();
        const auto status = step.value(QStringLiteral("status")).toString();
        if (status == QLatin1String("satisfied") ||
            status == QLatin1String("blocked"))
            continue;

        if (!step.value(QStringLiteral("requires_tool")).toBool())
            return QStringLiteral(
                       "The next checklist step is '%1'. It does not require "
                       "a tool; complete it before requesting another tool "
                       "call.")
                .arg(step.value(QStringLiteral("description")).toString());

        if (activeRun_->ledger.hasUnresolvedVerification())
            return QStringLiteral(
                       "The current checklist step '%1' cannot advance: %2")
                .arg(step.value(QStringLiteral("description")).toString())
                .arg(activeRun_->ledger.unresolvedVerificationReason());

        QJsonArray stepEvidence;
        for (auto sequence = activeRun_->currentPlanStepEvidenceStart;
             sequence <= evidenceSequence; ++sequence)
            stepEvidence.append(sequence);
        step.insert(QStringLiteral("status"), QStringLiteral("satisfied"));
        step.insert(QStringLiteral("evidence"), stepEvidence);
        activeRun_->completionSteps.replace(index, step);
        activeRun_->currentPlanStepEvidenceStart = evidenceSequence + 1;
        recordEvent(agent::EventType::TaskStepUpdated,
                    QStringLiteral("Task plan step verified."), {},
                    {{QStringLiteral("stepId"),
                      step.value(QStringLiteral("id")).toString()},
                     {QStringLiteral("evidenceSequence"), evidenceSequence}});
        if (index + 1 >= activeRun_->completionSteps.size())
            return QStringLiteral(
                       "The final task-plan step '%1' was verified by terminal "
                       "evidence %2. Return final after preparing the result; "
                       "do not execute another tool.")
                .arg(step.value(QStringLiteral("description")).toString())
                .arg(evidenceSequence);

        const auto next = activeRun_->completionSteps.at(index + 1).toObject();
        return QStringLiteral(
                   "The controller verified task-plan step '%1' with terminal "
                   "evidence %2. The next allowed step is '%3'; do not execute "
                   "later steps out of order.")
            .arg(step.value(QStringLiteral("description")).toString())
            .arg(evidenceSequence)
            .arg(next.value(QStringLiteral("description")).toString());
    }
    return {};
}

bool AgentController::hasSufficientCompletionEvidence() const
{
    if (!activeRun_ || activeRun_->pendingFinalCandidate.isEmpty() ||
        activeRun_->completionSteps.isEmpty() ||
        !unfinishedEvidenceReason(toolEvidence_, activeRun_->ledger).isEmpty())
        return false;

    auto requiredToolSteps = 0;
    for (const auto& value : activeRun_->completionSteps)
        if (value.toObject().value(QStringLiteral("requires_tool")).toBool())
            ++requiredToolSteps;

    auto successfulTerminalEvidence = 0;
    for (const auto& evidence : toolEvidence_)
        if (evidence.value(QStringLiteral("outcome")).toString() ==
                QLatin1String("success") &&
            evidence.value(QStringLiteral("terminal")).toBool())
            ++successfulTerminalEvidence;

    return successfulTerminalEvidence >= requiredToolSteps;
}

void AgentController::handleCompletionReview(const agent::Action& action,
                                             const QByteArray& rawAction)
{
    if (!activeRun_) return;
    auto normalizedAction = action;
    QStringList omittedStepIds;
    QStringList unexpectedStepIds;
    QJsonArray authoritativeSteps;
    for (const auto& plannedValue : activeRun_->completionSteps)
    {
        const auto planned = plannedValue.toObject();
        const auto plannedId = planned.value(QStringLiteral("id")).toString();
        auto reviewed = std::find_if(action.completionSteps.cbegin(),
                                     action.completionSteps.cend(),
                                     [&plannedId](const QJsonValue& value)
                                     {
                                         return value.toObject()
                                                    .value(QStringLiteral("id"))
                                                    .toString() == plannedId;
                                     });
        if (reviewed != action.completionSteps.cend())
        {
            auto reviewedStep = reviewed->toObject();
            reviewedStep.insert(QStringLiteral("description"),
                                planned.value(QStringLiteral("description")));
            reviewedStep.insert(QStringLiteral("requires_tool"),
                                planned.value(QStringLiteral("requires_tool")));
            authoritativeSteps.append(reviewedStep);
            continue;
        }

        auto restored = planned;
        if (!restored.contains(QStringLiteral("status")))
            restored.insert(QStringLiteral("status"),
                            QStringLiteral("pending"));
        if (!restored.contains(QStringLiteral("evidence")))
            restored.insert(QStringLiteral("evidence"), QJsonArray{});
        authoritativeSteps.append(restored);
        omittedStepIds.append(plannedId);
    }
    for (const auto& reviewedValue : action.completionSteps)
    {
        const auto reviewedId =
            reviewedValue.toObject().value(QStringLiteral("id")).toString();
        const auto recorded =
            std::any_of(activeRun_->completionSteps.cbegin(),
                        activeRun_->completionSteps.cend(),
                        [&reviewedId](const QJsonValue& value)
                        {
                            return value.toObject()
                                       .value(QStringLiteral("id"))
                                       .toString() == reviewedId;
                        });
        if (!recorded) unexpectedStepIds.append(reviewedId);
    }
    if (!omittedStepIds.isEmpty())
    {
        if (activeRun_->completionPlanDriftRepairs >= 1)
        {
            retryCompletionReview(
                rawAction,
                QStringLiteral(
                    "Completion review repeatedly replaced or omitted "
                    "recorded task-plan steps."));
            return;
        }
        ++activeRun_->completionPlanDriftRepairs;
        activeRun_->inferenceMessages.append(
            {chat::Role::Assistant, QString::fromUtf8(rawAction)});
        activeRun_->inferenceMessages.append(
            AgentPromptBuilder::completionPlanDriftMessage(
                activeRun_->completionSteps));
        recordEvent(
            agent::EventType::RecoveryStarted,
            QStringLiteral(
                "Restoring the completion review to the recorded task plan."),
            {},
            {{QStringLiteral("omittedStepCount"), omittedStepIds.size()},
             {QStringLiteral("unexpectedStepCount"),
              unexpectedStepIds.size()}});
        requestDecision();
        return;
    }

    for (const auto& reviewedValue : action.completionSteps)
    {
        const auto reviewedId =
            reviewedValue.toObject().value(QStringLiteral("id")).toString();
        if (unexpectedStepIds.contains(reviewedId))
            authoritativeSteps.append(reviewedValue);
    }
    normalizedAction.completionSteps = authoritativeSteps;

    const auto validationError = validateCompletionReview(normalizedAction);
    if (!validationError.isEmpty())
    {
        retryCompletionReview(rawAction, validationError);
        return;
    }

    activeRun_->completionSteps = normalizedAction.completionSteps;
    ++activeRun_->completionReviewSuccesses;
    activeRun_->awaitingCompletionReview = false;
    activeRun_->completionReviewFailures = 0;
    qInfo().noquote()
        << QStringLiteral(
               "Agent completion review accepted: run=%1 verdict=%2 steps=%3")
               .arg(activeRun_->id, normalizedAction.completionVerdict)
               .arg(activeRun_->completionSteps.size());
    recordEvent(
        agent::EventType::TaskStepUpdated,
        QStringLiteral("Task step status updated."), {},
        {{QStringLiteral("verdict"), normalizedAction.completionVerdict},
         {QStringLiteral("stepCount"), activeRun_->completionSteps.size()}});
    if (normalizedAction.completionVerdict == QLatin1String("complete"))
    {
        const auto content =
            std::exchange(activeRun_->pendingFinalCandidate, QString{});
        completeRun(content);
        return;
    }
    activeRun_->pendingFinalCandidate.clear();
    if (normalizedAction.completionVerdict == QLatin1String("blocked"))
    {
        completeRun(normalizedAction.completionDetail);
        return;
    }

    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::completionContinuationMessage(
            activeRun_->completionSteps, normalizedAction.completionDetail));
    requestDecision();
}

void AgentController::executeTool(const agent::Action& action)
{
    if (!activeRun_) return;
    const auto signature = toolCallSignature(action);
    if (!pollableToolCallSignature_.isEmpty() &&
        signature == pollableToolCallSignature_ && lastPollCompletedAtMs_ > 0)
    {
        const auto elapsed =
            QDateTime::currentMSecsSinceEpoch() - lastPollCompletedAtMs_;
        const auto remaining = minimumPollIntervalMilliseconds - elapsed;
        if (remaining > 0)
        {
            pendingPollAction_ = action;
            setState(AgentRun::State::ExecutingTool);
            pollTimer_->start(static_cast<int>(remaining));
            return;
        }
    }
    if (!pollableToolCallSignature_.isEmpty() &&
        signature == pollableToolCallSignature_)
        ++activeRun_->pollRequests;
    ++activeRun_->executedToolCalls;
    activeToolAction_ = action;
    activeToolCallSignature_ = signature;
    setState(AgentRun::State::ExecutingTool);
    recordEvent(agent::EventType::ToolStarted,
                QStringLiteral("Tool call started."), action.toolName,
                action.arguments);
    activeRun_->toolRequestId =
        dependencies_.callTool(action.toolName, action.arguments);
    if (activeRun_->toolRequestId.isEmpty())
        failRun(QStringLiteral("tool_call_failed"),
                QStringLiteral("Tool call could not be started."));
}

void AgentController::executePendingPoll()
{
    if (!hasActiveRun() || !pendingPollAction_.has_value()) return;
    const auto action = std::exchange(pendingPollAction_, std::nullopt);
    executeTool(*action);
}

void AgentController::retryTaskPlan(const QByteArray& rawAction,
                                    const QString& errorMessage)
{
    if (!activeRun_) return;
    if (activeRun_->taskPlanFailures >= 1)
    {
        qWarning().noquote()
            << QStringLiteral(
                   "Agent task plan failed: run=%1 reason=%2 action=%3")
                   .arg(activeRun_->id, singleLine(errorMessage),
                        singleLine(QString::fromUtf8(rawAction)).left(2'048));
        failRun(QStringLiteral("task_plan_failed"), errorMessage);
        return;
    }
    ++activeRun_->taskPlanFailures;
    qWarning().noquote()
        << QStringLiteral(
               "Agent task plan correction: run=%1 attempt=%2 reason=%3 "
               "action=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->taskPlanFailures)
               .arg(singleLine(errorMessage),
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::taskPlanCorrectionMessage(errorMessage));
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Repairing the task plan."));
    requestDecision();
}

void AgentController::retryCompletionReview(const QByteArray& rawAction,
                                            const QString& errorMessage)
{
    if (!activeRun_) return;
    if (activeRun_->completionReviewFailures >= 1)
    {
        qWarning().noquote()
            << QStringLiteral(
                   "Agent completion review failed: run=%1 reason=%2 "
                   "action=%3")
                   .arg(activeRun_->id, singleLine(errorMessage),
                        singleLine(QString::fromUtf8(rawAction)).left(2'048));
        if (hasSufficientCompletionEvidence())
        {
            const auto content =
                std::exchange(activeRun_->pendingFinalCandidate, QString{});
            activeRun_->awaitingCompletionReview = false;
            recordEvent(
                agent::EventType::Warning,
                QStringLiteral(
                    "Completion review formatting failed after correction; "
                    "using the prepared final answer because successful "
                    "terminal evidence covers every tool-required step."));
            completeRun(content);
            return;
        }
        failRun(QStringLiteral("completion_unverified"), errorMessage);
        return;
    }
    ++activeRun_->completionReviewFailures;
    qWarning().noquote()
        << QStringLiteral(
               "Agent completion review correction: run=%1 attempt=%2 "
               "reason=%3 action=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->completionReviewFailures)
               .arg(singleLine(errorMessage),
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::completionReviewCorrectionMessage(errorMessage));
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Repairing the completion review."));
    requestDecision();
}

void AgentController::retryUnfinishedFinal(const QByteArray& rawAction,
                                           const QString& errorMessage)
{
    if (!activeRun_) return;
    if (activeRun_->completionReviewFailures >= 1)
    {
        qWarning().noquote()
            << QStringLiteral(
                   "Agent unfinished final failed: run=%1 reason=%2 action=%3")
                   .arg(activeRun_->id, singleLine(errorMessage),
                        singleLine(QString::fromUtf8(rawAction)).left(2'048));
        failRun(QStringLiteral("completion_unverified"), errorMessage);
        return;
    }
    ++activeRun_->completionReviewFailures;
    qWarning().noquote()
        << QStringLiteral(
               "Agent unfinished final correction: run=%1 attempt=%2 "
               "reason=%3 action=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->completionReviewFailures)
               .arg(singleLine(errorMessage),
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::unfinishedFinalMessage(errorMessage));
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Checking unfinished task steps."));
    requestDecision();
}

void AgentController::retryInvalidAction(const QByteArray& rawAction,
                                         const QString& errorMessage)
{
    if (!activeRun_) return;
    if (activeRun_->taskPlanRequired)
    {
        retryTaskPlan(rawAction, errorMessage);
        return;
    }
    if (activeRun_->awaitingCompletionReview)
    {
        retryCompletionReview(rawAction, errorMessage);
        return;
    }
    if (activeRun_->repairAttempts >= 1)
    {
        failRun(QStringLiteral("invalid_agent_action"), errorMessage);
        return;
    }
    ++activeRun_->repairAttempts;
    qWarning().noquote()
        << QStringLiteral(
               "Agent action correction: run=%1 attempt=%2 reason=%3 "
               "action=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->repairAttempts)
               .arg(singleLine(errorMessage),
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::correctionMessage(errorMessage));
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Repairing an invalid agent action."));
    requestDecision();
}

void AgentController::retryInvalidToolAction(
    const QByteArray& rawAction, const agent::ToolValidationIssue& issue,
    const agent::ToolDefinition& tool, const QJsonObject& arguments)
{
    if (!activeRun_) return;
    if (activeRun_->consecutiveValidationFailures >= 1)
    {
        failRun(QStringLiteral("invalid_tool_arguments"), issue.message);
        return;
    }
    ++activeRun_->validationRepairs;
    ++activeRun_->consecutiveValidationFailures;
    qWarning().noquote()
        << QStringLiteral(
               "Agent tool validation correction: run=%1 repairs=%2 "
               "reason=%3 tool=%4 path=%5 keyword=%6 action=%7")
               .arg(activeRun_->id)
               .arg(activeRun_->validationRepairs)
               .arg(singleLine(issue.message), tool.qualifiedName,
                    issue.instancePath, issue.keyword,
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::toolValidationCorrectionMessage(tool, arguments,
                                                            issue));
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Repairing invalid tool arguments."),
                tool.qualifiedName);
    requestDecision();
}

void AgentController::retryNoProgressAction(const QByteArray& rawAction,
                                            const QString& errorMessage)
{
    if (!activeRun_) return;
    if (activeRun_->stagnationRecoveries >= 1)
    {
        failRun(QStringLiteral("agent_stalled"), errorMessage);
        return;
    }
    ++activeRun_->stagnationRecoveries;
    qWarning().noquote()
        << QStringLiteral(
               "Agent stagnation recovery: run=%1 attempt=%2 reason=%3 "
               "action=%4")
               .arg(activeRun_->id)
               .arg(activeRun_->stagnationRecoveries)
               .arg(singleLine(errorMessage),
                    singleLine(QString::fromUtf8(rawAction)).left(2'048));
    activeRun_->inferenceMessages.append(
        {chat::Role::Assistant, QString::fromUtf8(rawAction)});
    activeRun_->inferenceMessages.append(
        AgentPromptBuilder::noProgressMessage(errorMessage));
    recordEvent(agent::EventType::RecoveryStarted,
                QStringLiteral("Recovering from a repeated action."));
    requestDecision();
}

QString AgentController::contractRecoveryError(
    const agent::Action& action) const
{
    const auto recovery =
        contractRecoveries_.constFind(contractFailureKey(action));
    if (recovery == contractRecoveries_.cend()) return {};
    if (recovery->discoveryTool.isEmpty())
        return QStringLiteral(
            "The same tool and target already returned an argument "
            "contract error. No matching contract-discovery tool is "
            "available, so do not try another field-path variation. "
            "Use a different supported operation or report the "
            "blocker.");
    auto requirement = QStringLiteral(
                           "The same tool and target already returned an "
                           "argument contract error. Before retrying %1, call "
                           "%2")
                           .arg(recovery->failedTool, recovery->discoveryTool);
    if (!recovery->itemType.isEmpty())
        requirement +=
            QStringLiteral(" with item_type=%1").arg(recovery->itemType);
    if (!recovery->selector.isEmpty())
        requirement += QStringLiteral(
                           " and use the selector required by that model for "
                           "the target previously identified as %1")
                           .arg(recovery->selector);
    return requirement + QStringLiteral(
                             ". Do not guess another edit shape before the "
                             "contract lookup succeeds.");
}

QString AgentController::modelContractError(const agent::Action& action) const
{
    if (unqualifiedToolName(action.toolName) != QLatin1String("edit_object"))
        return {};
    const auto changesValue = action.arguments.value(QStringLiteral("changes"));
    if (!changesValue.isObject()) return {};

    QString serverId;
    const auto definition =
        std::find_if(availableTools_.cbegin(), availableTools_.cend(),
                     [&action](const agent::ToolDefinition& candidate)
                     { return candidate.qualifiedName == action.toolName; });
    if (definition != availableTools_.cend()) serverId = definition->serverId;
    const auto itemType = actionItemType(action);
    const auto selector = actionSelector(action);
    auto allowedPaths = modelContractPaths_.value(
        modelContractKey(serverId, itemType, selector));
    if (allowedPaths.isEmpty())
        allowedPaths =
            modelContractPaths_.value(modelContractKey(serverId, itemType, {}));

    for (const auto& path : changesValue.toObject().keys())
    {
        const auto diagnosticPath = path.startsWith(QLatin1String("/changes/"));
        if (!isValidModelJsonPointer(path) || diagnosticPath)
        {
            auto message = QStringLiteral(
                "edit_object changes keys must be model JSON Pointers that "
                "start with '/', for example "
                "'/density/isotropic/fixedValue'. The diagnostic "
                "instance_path '/changes/~1density~1isotropic~1fixedValue' "
                "describes a location inside the tool arguments; '~1' does "
                "not replace '/' in the actual changes key. Do not copy an "
                "instance_path into changes.");
            const auto summary = summarizedAllowedPaths(allowedPaths, path);
            if (!summary.isEmpty())
                message += QStringLiteral(
                               " Exact paths returned by "
                               "describe_model for this target: ") +
                           summary + QLatin1Char('.');
            return message;
        }
        if (!allowedPaths.isEmpty() && !allowedPaths.contains(path))
        {
            auto message =
                QStringLiteral(
                    "edit_object path '%1' was not returned by "
                    "describe_model for this target. Use an exact "
                    "fields[].path "
                    "model path. Do not insert field values such as "
                    "'constValue' into the path. Exact allowed paths: ")
                    .arg(path);
            message +=
                summarizedAllowedPaths(allowedPaths, path) + QLatin1Char('.');
            return message;
        }
    }
    return {};
}

QString AgentController::registerContractFailure(
    const agent::Action& action, const agent::ToolResult& result,
    bool& exhausted)
{
    const auto key = contractFailureKey(action);
    const auto failures = contractFailureCounts_.value(key) + 1;
    contractFailureCounts_.insert(key, failures);
    exhausted = failures >= maximumContractFailuresPerTarget;

    QString serverId;
    const auto failedDefinition =
        std::find_if(availableTools_.cbegin(), availableTools_.cend(),
                     [&action](const agent::ToolDefinition& candidate)
                     { return candidate.qualifiedName == action.toolName; });
    if (failedDefinition != availableTools_.cend())
        serverId = failedDefinition->serverId;
    QString discoveryTool;
    const auto discovery =
        std::find_if(availableTools_.cbegin(), availableTools_.cend(),
                     [&serverId](const agent::ToolDefinition& candidate)
                     {
                         return candidate.serverId == serverId &&
                                unqualifiedToolName(candidate.qualifiedName) ==
                                    QLatin1String("describe_model");
                     });
    if (discovery != availableTools_.cend())
        discoveryTool = discovery->qualifiedName;

    const ContractRecovery recovery{action.toolName, discoveryTool,
                                    actionItemType(action),
                                    actionSelector(action)};
    contractRecoveries_.insert(key, recovery);
    if (exhausted)
        return QStringLiteral(
            "The argument contract failed again after recovery. The "
            "controller will stop this run instead of allowing more "
            "field-path guesses.");
    if (discoveryTool.isEmpty())
        return QStringLiteral(
                   "Error %1 is an argument-contract failure. No matching "
                   "describe_model tool is available. Do not retry the same "
                   "tool and target with guessed field paths; use another "
                   "supported operation or report the blocker.")
            .arg(result.errorCode);
    auto guidance =
        QStringLiteral(
            "Error %1 is an argument-contract failure. Before retrying %2, "
            "call %3")
            .arg(result.errorCode, action.toolName, discoveryTool);
    if (!recovery.itemType.isEmpty())
        guidance += QStringLiteral(" with item_type=%1").arg(recovery.itemType);
    return guidance + QStringLiteral(
                          ". Use the returned selector and exact JSON "
                          "Pointer fields[].path or model_path values; do not "
                          "guess another edit shape. An instance_path such as "
                          "'/changes/~1density~1isotropic~1fixedValue' is only "
                          "a diagnostic location in the tool arguments and "
                          "must never be used as a changes key.");
}

void AgentController::captureModelContract(const agent::Action& action,
                                           const agent::ToolResult& result)
{
    if (unqualifiedToolName(action.toolName) != QLatin1String("describe_model"))
        return;
    const auto paths = describedFieldPaths(result);
    if (paths.isEmpty()) return;

    QString serverId;
    const auto definition =
        std::find_if(availableTools_.cbegin(), availableTools_.cend(),
                     [&action](const agent::ToolDefinition& candidate)
                     { return candidate.qualifiedName == action.toolName; });
    if (definition != availableTools_.cend()) serverId = definition->serverId;
    const auto itemType = actionItemType(action);
    if (itemType.isEmpty()) return;
    modelContractPaths_.insert(
        modelContractKey(serverId, itemType, actionSelector(action)), paths);

    for (const auto& recovery : std::as_const(contractRecoveries_))
    {
        const auto discoverySatisfied =
            action.toolName == recovery.discoveryTool &&
            (recovery.itemType.isEmpty() ||
             recovery.itemType.compare(itemType, Qt::CaseInsensitive) == 0);
        if (discoverySatisfied)
            modelContractPaths_.insert(
                modelContractKey(serverId, itemType, recovery.selector), paths);
    }
}

void AgentController::resolveContractRecovery(const agent::Action& action)
{
    const auto itemType = actionItemType(action);
    for (auto recovery = contractRecoveries_.begin();
         recovery != contractRecoveries_.end();)
    {
        const auto discoverySatisfied =
            !recovery->discoveryTool.isEmpty() &&
            action.toolName == recovery->discoveryTool &&
            (recovery->itemType.isEmpty() ||
             recovery->itemType.compare(itemType, Qt::CaseInsensitive) == 0);
        if (discoverySatisfied)
            recovery = contractRecoveries_.erase(recovery);
        else
            ++recovery;
    }

    const auto key = contractFailureKey(action);
    if (contractFailureCounts_.contains(key))
    {
        contractRecoveries_.remove(key);
        contractFailureCounts_.remove(key);
    }
}

bool AgentController::isRequiredContractDiscovery(
    const agent::Action& action) const
{
    const auto itemType = actionItemType(action);
    return std::any_of(contractRecoveries_.cbegin(), contractRecoveries_.cend(),
                       [&action, &itemType](const ContractRecovery& recovery)
                       {
                           return !recovery.discoveryTool.isEmpty() &&
                                  action.toolName == recovery.discoveryTool &&
                                  (recovery.itemType.isEmpty() ||
                                   recovery.itemType.compare(
                                       itemType, Qt::CaseInsensitive) == 0);
                       });
}

bool AgentController::requiresContextResetApproval(
    const agent::Action& action) const
{
    return isContextResetToolName(action.toolName) &&
           (contextEstablished_ || hasStateChangesInContext_);
}

bool AgentController::isSameActiveContextOperation(
    const agent::Action& action) const
{
    if (!contextEstablished_ || activeContextScope_.isEmpty() ||
        !opensContext(action.toolName))
        return false;
    const auto requestedScope = requestedContextScope(action);
    return !requestedScope.isEmpty() && requestedScope == activeContextScope_;
}

QString AgentController::updateContextAfterSuccess(
    const agent::Action& action, const agent::ToolResult& result,
    ToolOperationKind completedOperationKind)
{
    if (!isContextResetToolName(action.toolName))
    {
        if (completedOperationKind == ToolOperationKind::Mutation)
            hasStateChangesInContext_ = true;
        return {};
    }

    const auto invalidatesPriorContext =
        contextEstablished_ || hasStateChangesInContext_;
    if (invalidatesPriorContext) invalidateContextEvidence();

    if (closesContext(action.toolName))
    {
        activeContextScope_.clear();
        contextEstablished_ = false;
        hasStateChangesInContext_ = false;
    }
    else
    {
        activeContextScope_ = contextScope(action, result);
        contextEstablished_ = true;
        hasStateChangesInContext_ =
            unqualifiedToolName(action.toolName) == QLatin1String("new_case");
    }

    if (!invalidatesPriorContext) return {};
    recordEvent(
        agent::EventType::Warning,
        QStringLiteral(
            "The active external context changed. Resources and evidence "
            "from the previous context were invalidated."),
        action.toolName);
    return QStringLiteral(
        "The active external context changed. The local controller removed "
        "all prior resource identifiers and completion evidence. Do not "
        "reuse identifiers from before this context transition.");
}

QString AgentController::staleResourceReference(
    const QJsonObject& arguments) const
{
    return findReferencedId(arguments, invalidatedResourceIds_);
}

void AgentController::invalidateContextEvidence()
{
    if (!activeRun_) return;
    for (const auto& resource : activeRun_->ledger.resources())
    {
        if (resource.stableId.isEmpty() ||
            invalidatedResourceIds_.contains(resource.stableId,
                                             Qt::CaseInsensitive))
            continue;
        invalidatedResourceIds_.append(resource.stableId);
    }
    while (invalidatedResourceIds_.size() > maximumInvalidatedResourceIds)
        invalidatedResourceIds_.removeFirst();

    activeRun_->ledger.clear();
    toolEvidence_.clear();
    completedToolCallHistory_.clear();
    contractRecoveries_.clear();
    contractFailureCounts_.clear();
    modelContractPaths_.clear();
    lastFailedToolCallSignature_.clear();
    pollableToolCallSignature_.clear();
    lastPollCompletedAtMs_ = 0;
    ++activeRun_->evidenceRevision;
    activeRun_->lastReviewedEvidenceRevision = -1;
    activeRun_->completionReviewsAtRevision = 0;
    activeRun_->completionReviewFailures = 0;
    activeRun_->completionPlanDriftRepairs = 0;
}

void AgentController::setState(AgentRun::State state)
{
    if (state_ == state) return;
    state_ = state;
    if (activeRun_) activeRun_->state = state;
    emit stateChanged(state_);
    emitProgressChanged();
}

void AgentController::recordEvent(agent::EventType type, const QString& message,
                                  const QString& toolName,
                                  const QJsonObject& data)
{
    if (!activeRun_) return;
    agent::Event event{activeRun_->id, type, message,
                       toolName,       data, QDateTime::currentDateTimeUtc()};
    activeRun_->events.append(event);
    auto logMessage = QStringLiteral("Agent event: run=%1 type=%2")
                          .arg(activeRun_->id, agent::eventTypeName(type));
    if (!toolName.isEmpty())
        logMessage += QStringLiteral(" tool=%1").arg(toolName);
    if (!message.isEmpty())
        logMessage += QStringLiteral(" message=%1").arg(singleLine(message));
    if (!data.isEmpty())
        logMessage += QStringLiteral(" data=%1").arg(eventDataSummary(data));
    qInfo().noquote() << logMessage;
    emit eventRecorded(event);
    emitProgressChanged();
}

void AgentController::emitProgressChanged()
{
    if (!activeRun_) return;
    emit progressChanged(progressSnapshot());
}

void AgentController::completeRun(const QString& content)
{
    if (!activeRun_) return;
    activeRun_->finishCode.clear();
    activeRun_->finishMessage.clear();
    setState(AgentRun::State::GeneratingAnswer);
    recordEvent(agent::EventType::AnswerStarted,
                QStringLiteral("Preparing final answer."));
    emit finalAnswerReady(activeRun_->id, content);
    conversationMessages_.append({chat::Role::User, activeRun_->userRequest});
    conversationMessages_.append({chat::Role::Assistant, content});
    setState(AgentRun::State::Completed);
    if (runTimer_->isActive()) runTimer_->stop();
    if (pollTimer_->isActive()) pollTimer_->stop();
    pendingPollAction_.reset();
    pollableToolCallSignature_.clear();
    lastPollCompletedAtMs_ = 0;
    recordEvent(agent::EventType::Completed,
                QStringLiteral("Agent run completed."));
    emit metricsReady(activeRun_->id,
                      AgentRunMetrics::fromRun(*activeRun_).toJson());
    emit runFinished(activeRun_->id, state_, {}, {});
}

void AgentController::failRun(const QString& code, const QString& message)
{
    if (!activeRun_ || isTerminal(state_)) return;
    const auto previousState = state_;
    const auto toolRequestId = activeRun_->toolRequestId;
    activeRun_->finishCode = code;
    activeRun_->finishMessage = message;
    setState(AgentRun::State::Failed);
    if (runTimer_->isActive()) runTimer_->stop();
    if (pollTimer_->isActive()) pollTimer_->stop();
    pendingApproval_.reset();
    activeToolAction_.reset();
    pendingPollAction_.reset();
    pollableToolCallSignature_.clear();
    lastPollCompletedAtMs_ = 0;
    activeRun_->toolRequestId.clear();
    if (previousState == AgentRun::State::Deciding &&
        dependencies_.cancelGeneration)
        dependencies_.cancelGeneration();
    if (previousState == AgentRun::State::ExecutingTool &&
        !toolRequestId.isEmpty() && dependencies_.cancelTool)
        dependencies_.cancelTool(toolRequestId);
    recordEvent(agent::EventType::Failed, message);
    emit metricsReady(activeRun_->id,
                      AgentRunMetrics::fromRun(*activeRun_).toJson());
    emit runFinished(activeRun_->id, state_, code, message);
}
}  // namespace qtllm::application
