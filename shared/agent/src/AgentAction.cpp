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
constexpr qsizetype maximumPlanDescriptionCharacters = 4'096;

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
    for (qsizetype index = 0; index < values.size(); ++index)
    {
        const auto value = values.at(index);
        const auto fieldPath = QStringLiteral("task_plan steps[%1]").arg(index);
        if (!value.isObject())
        {
            errorMessage =
                QStringLiteral("%1 must be an object.").arg(fieldPath);
            return false;
        }
        const auto step = value.toObject();
        auto id = step.value(QStringLiteral("id")).toString().trimmed();
        const auto description =
            step.value(QStringLiteral("description")).toString().trimmed();
        const auto descriptionValue = step.value(QStringLiteral("description"));
        const auto requiresToolValue =
            step.value(QStringLiteral("requires_tool"));
        const auto allowedToolsValue =
            step.value(QStringLiteral("allowed_tools"));
        if (!descriptionValue.isString() || description.isEmpty())
        {
            errorMessage =
                QStringLiteral("%1.description must be a non-empty string.")
                    .arg(fieldPath);
            return false;
        }
        if (description.size() > maximumPlanDescriptionCharacters)
        {
            errorMessage =
                QStringLiteral("%1.description exceeds %2 characters.")
                    .arg(fieldPath)
                    .arg(maximumPlanDescriptionCharacters);
            return false;
        }
        if (!requiresToolValue.isUndefined() && !requiresToolValue.isBool())
        {
            errorMessage = QStringLiteral("%1.requires_tool must be boolean.")
                               .arg(fieldPath);
            return false;
        }
        if (!allowedToolsValue.isUndefined() && !allowedToolsValue.isArray())
        {
            errorMessage = QStringLiteral("%1.allowed_tools must be an array.")
                               .arg(fieldPath);
            return false;
        }
        if (allowedToolsValue.isUndefined() &&
            (!requiresToolValue.isBool() || requiresToolValue.toBool()))
        {
            errorMessage = QStringLiteral(
                               "%1.allowed_tools is required for a "
                               "tool-required step.")
                               .arg(fieldPath);
            return false;
        }

        QSet<QString> allowedToolNames;
        QJsonArray allowedTools;
        for (const auto& toolValue : allowedToolsValue.toArray())
        {
            const auto toolName = toolValue.toString().trimmed();
            if (!toolValue.isString() || toolName.isEmpty() ||
                toolName.size() > 256)
            {
                errorMessage =
                    QStringLiteral(
                        "%1.allowed_tools must contain only non-empty "
                        "qualified tool names of at most 256 characters.")
                        .arg(fieldPath);
                return false;
            }
            if (allowedToolNames.contains(toolName)) continue;
            allowedToolNames.insert(toolName);
            allowedTools.append(toolName);
        }
        auto requiresTool = requiresToolValue.isBool()
                                ? requiresToolValue.toBool()
                                : !allowedTools.isEmpty();
        if (requiresTool != !allowedTools.isEmpty())
        {
            errorMessage =
                QStringLiteral(
                    "%1 requires a non-empty allowed_tools array exactly "
                    "when requires_tool is true.")
                    .arg(fieldPath);
            return false;
        }

        if (id.isEmpty() || id.size() > 64 || ids.contains(id))
        {
            id = QStringLiteral("step-%1").arg(index + 1);
            auto suffix = 2;
            const auto base = id;
            while (ids.contains(id))
                id = QStringLiteral("%1-%2").arg(base).arg(suffix++);
        }
        ids.insert(id);
        steps.append(
            QJsonObject{{QStringLiteral("id"), id},
                        {QStringLiteral("description"), description},
                        {QStringLiteral("requires_tool"), requiresTool},
                        {QStringLiteral("allowed_tools"), allowedTools}});
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
                "call_tool requires only a non-empty tool property and an "
                "object arguments property.");
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
root ::= ws (call-tool | task-plan | blocked | final) ws
call-tool ::= "{" ws "\"action\"" ws ":" ws "\"call_tool\"" ws "," ws "\"tool\"" ws ":" ws string ws "," ws "\"arguments\"" ws ":" ws object ws "}"
task-plan ::= "{" ws "\"action\"" ws ":" ws "\"task_plan\"" ws "," ws "\"steps\"" ws ":" ws "[" ws plan-step (ws "," ws plan-step)* ws "]" ws "," ws "\"ordered\"" ws ":" ws "true" ws "}"
plan-step ::= "{" ws "\"id\"" ws ":" ws string ws "," ws "\"description\"" ws ":" ws string ws "," ws "\"requires_tool\"" ws ":" ws boolean ws "," ws "\"allowed_tools\"" ws ":" ws string-array ws "}"
string-array ::= "[" ws (string (ws "," ws string)*)? ws "]"
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
