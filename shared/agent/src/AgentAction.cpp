#include "AgentAction.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

#include <utility>

namespace qtllm::agent
{
namespace
{
bool hasOnlyKeys(const QJsonObject& object,
                 std::initializer_list<QString> allowedKeys)
{
    if (object.size() != static_cast<qsizetype>(allowedKeys.size()))
        return false;
    for (const auto& key : allowedKeys)
    {
        if (!object.contains(key)) return false;
    }
    return true;
}

QString normalizedReviewStatus(QString status)
{
    status = status.trimmed().toCaseFolded();
    if (status == QLatin1String("satisfied") ||
        status == QLatin1String("pending") ||
        status == QLatin1String("blocked"))
        return status;
    if (status == QLatin1String("complete") ||
        status == QLatin1String("completed") || status == QLatin1String("done"))
        return QStringLiteral("satisfied");
    if (status == QLatin1String("incomplete") ||
        status == QLatin1String("unfinished") ||
        status == QLatin1String("todo"))
        return QStringLiteral("pending");
    return {};
}

bool parsePlanSteps(const QJsonArray& values, QJsonArray& steps,
                    QString& errorMessage)
{
    if (values.isEmpty() || values.size() > 32)
    {
        errorMessage =
            QStringLiteral("task_plan requires between 1 and 32 steps.");
        return false;
    }
    QSet<QString> ids;
    for (const auto& value : values)
    {
        const auto step = value.toObject();
        const auto id = step.value(QStringLiteral("id")).toString().trimmed();
        const auto description =
            step.value(QStringLiteral("description")).toString().trimmed();
        const auto requiresTool = step.value(QStringLiteral("requires_tool"));
        const auto allowedToolsValue =
            step.value(QStringLiteral("allowed_tools"));
        if (!value.isObject() ||
            !hasOnlyKeys(step,
                         {QStringLiteral("id"), QStringLiteral("description"),
                          QStringLiteral("requires_tool"),
                          QStringLiteral("allowed_tools")}) ||
            id.isEmpty() || id.size() > 64 || description.isEmpty() ||
            description.size() > 256 || !requiresTool.isBool() ||
            !allowedToolsValue.isArray() || ids.contains(id))
        {
            errorMessage = QStringLiteral(
                "Each task_plan step requires a unique non-empty id, a "
                "description of at most 256 characters, and a boolean "
                "requires_tool property plus an allowed_tools array.");
            return false;
        }
        QSet<QString> allowedToolNames;
        QJsonArray allowedTools;
        for (const auto& toolValue : allowedToolsValue.toArray())
        {
            const auto toolName = toolValue.toString().trimmed();
            if (!toolValue.isString() || toolName.isEmpty() ||
                toolName.size() > 256 || allowedToolNames.contains(toolName))
            {
                errorMessage = QStringLiteral(
                    "task_plan allowed_tools values must be unique, "
                    "non-empty qualified tool names.");
                return false;
            }
            allowedToolNames.insert(toolName);
            allowedTools.append(toolName);
        }
        if ((requiresTool.toBool() && allowedTools.isEmpty()) ||
            (!requiresTool.toBool() && !allowedTools.isEmpty()))
        {
            errorMessage = QStringLiteral(
                "A tool-required task_plan step needs at least one "
                "allowed_tools entry; a non-tool step must use an empty "
                "allowed_tools array.");
            return false;
        }
        ids.insert(id);
        steps.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("description"), description},
            {QStringLiteral("requires_tool"), requiresTool.toBool()},
            {QStringLiteral("allowed_tools"), allowedTools}});
    }
    return true;
}

bool parseReviewSteps(const QJsonArray& values, QJsonArray& steps,
                      QString& errorMessage)
{
    if (values.isEmpty() || values.size() > 32)
    {
        errorMessage = QStringLiteral(
            "review_completion requires between 1 and 32 steps.");
        return false;
    }
    QSet<QString> ids;
    for (const auto& value : values)
    {
        const auto step = value.toObject();
        const auto id = step.value(QStringLiteral("id")).toString().trimmed();
        const auto description =
            step.value(QStringLiteral("description")).toString().trimmed();
        const auto requiresTool = step.value(QStringLiteral("requires_tool"));
        const auto status = normalizedReviewStatus(
            step.value(QStringLiteral("status")).toString());
        const auto evidence = step.value(QStringLiteral("evidence"));
        if (!value.isObject() ||
            !hasOnlyKeys(
                step, {QStringLiteral("id"), QStringLiteral("description"),
                       QStringLiteral("requires_tool"),
                       QStringLiteral("status"), QStringLiteral("evidence")}) ||
            id.isEmpty() || id.size() > 64 || description.isEmpty() ||
            description.size() > 256 || !requiresTool.isBool() ||
            status.isEmpty() || !evidence.isArray() || ids.contains(id))
        {
            errorMessage = QStringLiteral(
                "Each review_completion step requires the original id, "
                "description, requires_tool flag, a satisfied, pending, or "
                "blocked status, and an evidence array.");
            return false;
        }

        QSet<int> evidenceIds;
        QJsonArray normalizedEvidence;
        for (const auto& item : evidence.toArray())
        {
            const auto sequence = item.toInt(-1);
            if (!item.isDouble() || sequence <= 0 ||
                item.toDouble() != static_cast<double>(sequence) ||
                evidenceIds.contains(sequence))
            {
                errorMessage = QStringLiteral(
                    "review_completion evidence values must be unique "
                    "positive integer tool-call sequence numbers.");
                return false;
            }
            evidenceIds.insert(sequence);
            normalizedEvidence.append(sequence);
        }
        ids.insert(id);
        steps.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("description"), description},
            {QStringLiteral("requires_tool"), requiresTool.toBool()},
            {QStringLiteral("status"), status},
            {QStringLiteral("evidence"), normalizedEvidence}});
    }
    return true;
}
}  // namespace

bool parseAction(const QByteArray& json, Action& action, QString& errorMessage)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = QStringLiteral("Agent action must be a JSON object: %1")
                           .arg(parseError.errorString());
        return false;
    }

    const auto object = document.object();
    const auto actionValue = object.value(QStringLiteral("action"));
    if (!actionValue.isString())
    {
        errorMessage =
            QStringLiteral("Agent action requires a string action property.");
        return false;
    }

    if (actionValue.toString() == QStringLiteral("call_tool"))
    {
        const auto toolValue = object.value(QStringLiteral("tool"));
        const auto argumentsValue = object.value(QStringLiteral("arguments"));
        const auto planStepId = object.value(QStringLiteral("plan_step_id"));
        const auto completesPlanStep =
            object.value(QStringLiteral("completes_plan_step"));
        auto hasOnlyCallKeys = true;
        for (auto item = object.constBegin(); item != object.constEnd(); ++item)
            hasOnlyCallKeys =
                hasOnlyCallKeys &&
                (item.key() == QLatin1String("action") ||
                 item.key() == QLatin1String("tool") ||
                 item.key() == QLatin1String("arguments") ||
                 item.key() == QLatin1String("plan_step_id") ||
                 item.key() == QLatin1String("completes_plan_step"));
        const auto hasValidKeys = hasOnlyCallKeys && object.size() >= 3 &&
                                  object.size() <= 5 &&
                                  object.contains(QStringLiteral("action")) &&
                                  object.contains(QStringLiteral("tool")) &&
                                  object.contains(QStringLiteral("arguments"));
        if (!hasValidKeys || !toolValue.isString() ||
            toolValue.toString().trimmed().isEmpty() ||
            !argumentsValue.isObject() ||
            (!planStepId.isUndefined() && !planStepId.isString()) ||
            (!completesPlanStep.isUndefined() && !completesPlanStep.isBool()))
        {
            errorMessage = QStringLiteral(
                "call_tool requires only non-empty tool and object arguments "
                "properties, plus optional string plan_step_id and boolean "
                "completes_plan_step.");
            return false;
        }

        action = {};
        action.type = ActionType::CallTool;
        action.toolName = toolValue.toString().trimmed();
        action.planStepId = planStepId.toString().trimmed();
        if (!completesPlanStep.isUndefined())
            action.completesPlanStep = completesPlanStep.toBool();
        action.arguments = argumentsValue.toObject();
        return true;
    }

    if (actionValue.toString() == QStringLiteral("task_plan"))
    {
        const auto stepsValue = object.value(QStringLiteral("steps"));
        const auto orderedValue = object.value(QStringLiteral("ordered"));
        auto hasOnlyPlanKeys = true;
        for (auto item = object.constBegin(); item != object.constEnd(); ++item)
            hasOnlyPlanKeys =
                hasOnlyPlanKeys && (item.key() == QLatin1String("action") ||
                                    item.key() == QLatin1String("steps") ||
                                    item.key() == QLatin1String("ordered"));
        QJsonArray steps;
        if (!hasOnlyPlanKeys || object.size() < 2 || object.size() > 3 ||
            !stepsValue.isArray() ||
            (!orderedValue.isUndefined() && !orderedValue.isBool()) ||
            !parsePlanSteps(stepsValue.toArray(), steps, errorMessage))
        {
            if (errorMessage.isEmpty())
                errorMessage = QStringLiteral(
                    "task_plan requires only a valid steps array.");
            return false;
        }
        action = {};
        action.type = ActionType::TaskPlan;
        action.completionSteps = std::move(steps);
        if (!orderedValue.isUndefined())
            action.orderedPlan = orderedValue.toBool();
        return true;
    }

    if (actionValue.toString() == QStringLiteral("review_completion"))
    {
        const auto verdict =
            object.value(QStringLiteral("verdict")).toString().trimmed();
        const auto stepsValue = object.value(QStringLiteral("steps"));
        const auto detail =
            object.value(QStringLiteral("detail")).toString().trimmed();
        QJsonArray steps;
        if (!hasOnlyKeys(object,
                         {QStringLiteral("action"), QStringLiteral("verdict"),
                          QStringLiteral("steps"), QStringLiteral("detail")}) ||
            (verdict != QLatin1String("complete") &&
             verdict != QLatin1String("continue") &&
             verdict != QLatin1String("blocked")) ||
            !stepsValue.isArray() || detail.isEmpty() ||
            detail.size() > 2'048 ||
            !parseReviewSteps(stepsValue.toArray(), steps, errorMessage))
        {
            if (errorMessage.isEmpty())
                errorMessage = QStringLiteral(
                    "review_completion requires a complete, continue, or "
                    "blocked verdict, valid steps, and a non-empty detail.");
            return false;
        }

        const auto expectedStatus =
            verdict == QLatin1String("complete")   ? QStringLiteral("satisfied")
            : verdict == QLatin1String("continue") ? QStringLiteral("pending")
                                                   : QStringLiteral("blocked");
        auto hasExpectedStatus = false;
        auto allSatisfied = true;
        for (const auto& value : steps)
        {
            const auto status =
                value.toObject().value(QStringLiteral("status")).toString();
            hasExpectedStatus = hasExpectedStatus || status == expectedStatus;
            allSatisfied = allSatisfied && status == QLatin1String("satisfied");
        }
        if ((verdict == QLatin1String("complete") && !allSatisfied) ||
            (verdict != QLatin1String("complete") && !hasExpectedStatus))
        {
            errorMessage = QStringLiteral(
                "review_completion verdict does not match its step statuses.");
            return false;
        }

        action = {};
        action.type = ActionType::ReviewCompletion;
        action.completionVerdict = verdict;
        action.completionSteps = std::move(steps);
        action.completionDetail = detail;
        return true;
    }

    if (actionValue.toString() == QStringLiteral("review_plan_step"))
    {
        const auto stepId =
            object.value(QStringLiteral("step_id")).toString().trimmed();
        const auto status =
            object.value(QStringLiteral("status")).toString().trimmed();
        const auto evidenceValue = object.value(QStringLiteral("evidence"));
        const auto detail =
            object.value(QStringLiteral("detail")).toString().trimmed();
        if (!hasOnlyKeys(object,
                         {QStringLiteral("action"), QStringLiteral("step_id"),
                          QStringLiteral("status"), QStringLiteral("evidence"),
                          QStringLiteral("detail")}) ||
            stepId.isEmpty() || stepId.size() > 64 ||
            (status != QLatin1String("satisfied") &&
             status != QLatin1String("pending")) ||
            !evidenceValue.isArray() || detail.isEmpty() ||
            detail.size() > 2'048)
        {
            errorMessage = QStringLiteral(
                "review_plan_step requires the current step_id, a satisfied "
                "or pending status, an evidence array, and non-empty detail.");
            return false;
        }
        QSet<int> evidenceIds;
        QJsonArray evidence;
        for (const auto& value : evidenceValue.toArray())
        {
            const auto sequence = value.toInt(-1);
            if (!value.isDouble() || sequence <= 0 ||
                value.toDouble() != static_cast<double>(sequence) ||
                evidenceIds.contains(sequence))
            {
                errorMessage = QStringLiteral(
                    "review_plan_step evidence values must be unique "
                    "positive integer tool-call sequence numbers.");
                return false;
            }
            evidenceIds.insert(sequence);
            evidence.append(sequence);
        }
        action = {};
        action.type = ActionType::ReviewPlanStep;
        action.planStepId = stepId;
        action.planStepReviewStatus = status;
        action.planStepReviewEvidence = evidence;
        action.completionDetail = detail;
        return true;
    }

    if (actionValue.toString() == QStringLiteral("blocked"))
    {
        const auto reason =
            object.value(QStringLiteral("reason")).toString().trimmed();
        const auto content =
            object.value(QStringLiteral("content")).toString().trimmed();
        static const QSet<QString> reasons{
            QStringLiteral("missing_input"), QStringLiteral("authorization"),
            QStringLiteral("external_failure"), QStringLiteral("unsupported")};
        if (!hasOnlyKeys(object,
                         {QStringLiteral("action"), QStringLiteral("reason"),
                          QStringLiteral("content")}) ||
            !reasons.contains(reason) || content.isEmpty())
        {
            errorMessage = QStringLiteral(
                "blocked requires only a missing_input, authorization, "
                "external_failure, or unsupported reason and non-empty "
                "content.");
            return false;
        }

        action = {};
        action.type = ActionType::Blocked;
        action.blockReason = reason;
        action.content = content;
        return true;
    }

    if (actionValue.toString() == QStringLiteral("final"))
    {
        const auto contentValue = object.value(QStringLiteral("content"));
        if (!hasOnlyKeys(object, {QStringLiteral("action"),
                                  QStringLiteral("content")}) ||
            !contentValue.isString() ||
            contentValue.toString().trimmed().isEmpty())
        {
            errorMessage = QStringLiteral(
                "final requires only a non-empty content property.");
            return false;
        }

        action = {};
        action.type = ActionType::Final;
        action.content = contentValue.toString().trimmed();
        return true;
    }

    errorMessage =
        QStringLiteral("Unknown agent action: %1").arg(actionValue.toString());
    return false;
}

QByteArray actionGrammar()
{
    return QByteArrayLiteral(R"GBNF(
root ::= ws (call-tool | task-plan | review-plan-step | review-completion | blocked | final) ws
call-tool ::= "{" ws "\"action\"" ws ":" ws "\"call_tool\"" ws "," ws "\"tool\"" ws ":" ws string ws "," ws "\"arguments\"" ws ":" ws object ws "," ws "\"plan_step_id\"" ws ":" ws string ws "," ws "\"completes_plan_step\"" ws ":" ws boolean ws "}"
task-plan ::= "{" ws "\"action\"" ws ":" ws "\"task_plan\"" ws "," ws "\"steps\"" ws ":" ws array ws "," ws "\"ordered\"" ws ":" ws "true" ws "}"
review-plan-step ::= "{" ws "\"action\"" ws ":" ws "\"review_plan_step\"" ws "," ws "\"step_id\"" ws ":" ws string ws "," ws "\"status\"" ws ":" ws plan-step-review-status ws "," ws "\"evidence\"" ws ":" ws evidence-array ws "," ws "\"detail\"" ws ":" ws string ws "}"
plan-step-review-status ::= "\"satisfied\"" | "\"pending\""
review-completion ::= "{" ws "\"action\"" ws ":" ws "\"review_completion\"" ws "," ws "\"verdict\"" ws ":" ws completion-verdict ws "," ws "\"steps\"" ws ":" ws review-steps ws "," ws "\"detail\"" ws ":" ws string ws "}"
completion-verdict ::= "\"complete\"" | "\"continue\"" | "\"blocked\""
review-steps ::= "[" ws review-step (ws "," ws review-step)* ws "]"
review-step ::= "{" ws "\"id\"" ws ":" ws string ws "," ws "\"description\"" ws ":" ws string ws "," ws "\"requires_tool\"" ws ":" ws boolean ws "," ws "\"status\"" ws ":" ws review-status ws "," ws "\"evidence\"" ws ":" ws evidence-array ws "}"
review-status ::= "\"satisfied\"" | "\"pending\"" | "\"blocked\""
evidence-array ::= "[" ws (positive-integer (ws "," ws positive-integer)*)? ws "]"
positive-integer ::= [1-9] [0-9]*
blocked ::= "{" ws "\"action\"" ws ":" ws "\"blocked\"" ws "," ws "\"reason\"" ws ":" ws block-reason ws "," ws "\"content\"" ws ":" ws string ws "}"
block-reason ::= "\"missing_input\"" | "\"authorization\"" | "\"external_failure\"" | "\"unsupported\""
final ::= "{" ws "\"action\"" ws ":" ws "\"final\"" ws "," ws "\"content\"" ws ":" ws string ws "}"
value ::= object | array | string | number | "true" | "false" | "null"
boolean ::= "true" | "false"
object ::= "{" ws (member (ws "," ws member)*)? ws "}"
member ::= string ws ":" ws value
array ::= "[" ws (value (ws "," ws value)*)? ws "]"
string ::= "\"" characters "\""
characters ::= ([^"\\\x7F\x00-\x1F] | "\\" escape)*
escape ::= ["\\/bfnrt] | "u" hex hex hex hex
hex ::= [0-9a-fA-F]
number ::= "-"? ("0" | [1-9] [0-9]*) ("." [0-9]+)? ([eE] [+-]? [0-9]+)?
ws ::= [ \t\n\r]{0,8}
)GBNF");
}
}  // namespace qtllm::agent
