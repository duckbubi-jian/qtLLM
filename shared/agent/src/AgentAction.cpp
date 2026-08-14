#include "AgentAction.hpp"

#include <QJsonDocument>
#include <QJsonParseError>

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

        action = {ActionType::CallTool,
                  toolValue.toString().trimmed(),
                  argumentsValue.toObject(),
                  {}};
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

        action = {ActionType::Final, {}, {}, contentValue.toString().trimmed()};
        return true;
    }

    errorMessage =
        QStringLiteral("Unknown agent action: %1").arg(actionValue.toString());
    return false;
}

QByteArray actionGrammar()
{
    return QByteArrayLiteral(R"GBNF(
root ::= ws (call-tool | final) ws
call-tool ::= "{" ws "\"action\"" ws ":" ws "\"call_tool\"" ws "," ws "\"tool\"" ws ":" ws string ws "," ws "\"arguments\"" ws ":" ws object ws "}"
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
ws ::= [ \t\n\r]*
)GBNF");
}
}  // namespace qtllm::agent
