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
        if (!value.isObject() ||
            !hasOnlyKeys(step,
                         {QStringLiteral("id"), QStringLiteral("description"),
                          QStringLiteral("requires_tool")}) ||
            id.isEmpty() || id.size() > 64 || description.isEmpty() ||
            description.size() > 256 || !requiresTool.isBool() ||
            ids.contains(id))
        {
            errorMessage = QStringLiteral(
                "Each task_plan step requires a unique non-empty id, a "
                "description of at most 256 characters, and a boolean "
                "requires_tool property.");
            return false;
        }
        ids.insert(id);
        steps.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("description"), description},
            {QStringLiteral("requires_tool"), requiresTool.toBool()}});
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
        const auto status =
            step.value(QStringLiteral("status")).toString().trimmed();
        const auto evidence = step.value(QStringLiteral("evidence"));
        if (!value.isObject() ||
            !hasOnlyKeys(
                step, {QStringLiteral("id"), QStringLiteral("description"),
                       QStringLiteral("requires_tool"),
                       QStringLiteral("status"), QStringLiteral("evidence")}) ||
            id.isEmpty() || id.size() > 64 || description.isEmpty() ||
            description.size() > 256 || !requiresTool.isBool() ||
            (status != QLatin1String("satisfied") &&
             status != QLatin1String("pending") &&
             status != QLatin1String("blocked")) ||
            !evidence.isArray() || ids.contains(id))
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
        if (!hasOnlyKeys(object,
                         {QStringLiteral("action"), QStringLiteral("tool"),
                          QStringLiteral("arguments")}) ||
            !toolValue.isString() || toolValue.toString().trimmed().isEmpty() ||
            !argumentsValue.isObject())
        {
            errorMessage = QStringLiteral(
                "call_tool requires only non-empty tool and object arguments "
                "properties.");
            return false;
        }

        action = {};
        action.type = ActionType::CallTool;
        action.toolName = toolValue.toString().trimmed();
        action.arguments = argumentsValue.toObject();
        return true;
    }

    if (actionValue.toString() == QStringLiteral("task_plan"))
    {
        const auto stepsValue = object.value(QStringLiteral("steps"));
        QJsonArray steps;
        if (!hasOnlyKeys(object,
                         {QStringLiteral("action"), QStringLiteral("steps")}) ||
            !stepsValue.isArray() ||
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
root ::= ws (call-tool | task-plan | review-completion | final) ws
call-tool ::= "{" ws "\"action\"" ws ":" ws "\"call_tool\"" ws "," ws "\"tool\"" ws ":" ws string ws "," ws "\"arguments\"" ws ":" ws object ws "}"
task-plan ::= "{" ws "\"action\"" ws ":" ws "\"task_plan\"" ws "," ws "\"steps\"" ws ":" ws array ws "}"
review-completion ::= "{" ws "\"action\"" ws ":" ws "\"review_completion\"" ws "," ws "\"verdict\"" ws ":" ws string ws "," ws "\"steps\"" ws ":" ws array ws "," ws "\"detail\"" ws ":" ws string ws "}"
final ::= "{" ws "\"action\"" ws ":" ws "\"final\"" ws "," ws "\"content\"" ws ":" ws string ws "}"
value ::= object | array | string | number | "true" | "false" | "null"
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
