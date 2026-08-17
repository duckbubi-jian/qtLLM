#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

namespace
{
void writeResponse(QFile& output, const QJsonValue& id,
                   const QJsonObject& result)
{
    const auto response =
        QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("result"), result}};
    output.write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
    output.flush();
}

void writeError(QFile& output, const QJsonValue& id, int code,
                const QString& message)
{
    const auto response =
        QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("error"),
                     QJsonObject{{QStringLiteral("code"), code},
                                 {QStringLiteral("message"), message}}}};
    output.write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
    output.flush();
}
}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    const auto invalid = arguments.contains(QStringLiteral("--invalid"));
    const auto exitImmediately = arguments.contains(QStringLiteral("--exit"));
    const auto noToolsCapability =
        arguments.contains(QStringLiteral("--no-tools-capability"));
    auto protocolVersion = QStringLiteral("2024-11-05");
    const auto protocolVersionPrefix = QStringLiteral("--protocol-version=");
    for (const auto& argument : arguments)
        if (argument.startsWith(protocolVersionPrefix))
            protocolVersion = argument.mid(protocolVersionPrefix.size());

    QFile input;
    QFile output;
    if (!input.open(stdin, QIODevice::ReadOnly) ||
        !output.open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered))
        return 2;
    if (exitImmediately) return 0;

    while (!input.atEnd())
    {
        const auto line = input.readLine().trimmed();
        if (line.isEmpty()) continue;
        if (invalid)
        {
            output.write("this is not json\n");
            output.flush();
            return 0;
        }

        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError ||
            !document.isObject())
            return 3;
        const auto request = document.object();
        const auto id = request.value(QStringLiteral("id"));
        const auto method = request.value(QStringLiteral("method")).toString();
        if (method.startsWith(QStringLiteral("notifications/"))) continue;
        if (method == QStringLiteral("initialize"))
        {
            const auto capabilities =
                noToolsCapability
                    ? QJsonObject{}
                    : QJsonObject{{QStringLiteral("tools"), QJsonObject{}}};
            writeResponse(
                output, id,
                {{QStringLiteral("protocolVersion"), protocolVersion},
                 {QStringLiteral("capabilities"), capabilities},
                 {QStringLiteral("serverInfo"),
                  QJsonObject{
                      {QStringLiteral("name"), QStringLiteral("fake")},
                      {QStringLiteral("version"), QStringLiteral("1")}}},
                 {QStringLiteral("instructions"),
                  QStringLiteral("Use fake tools carefully.")}});
        }
        else if (method == QStringLiteral("tools/list"))
        {
            QJsonArray tools;
            tools.append(QJsonObject{
                {QStringLiteral("name"), QStringLiteral("echo")},
                {QStringLiteral("description"),
                 QStringLiteral("Echo arguments")},
                {QStringLiteral("inputSchema"),
                 QJsonObject{
                     {QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("properties"),
                      QJsonObject{{QStringLiteral("value"),
                                   QJsonObject{{QStringLiteral("type"),
                                                QStringLiteral("string")}}}}},
                     {QStringLiteral("required"),
                      QJsonArray{QStringLiteral("value")}},
                     {QStringLiteral("additionalProperties"), false}}}});
            tools.append(
                QJsonObject{{QStringLiteral("name"), QStringLiteral("error")},
                            {QStringLiteral("description"),
                             QStringLiteral("Return an error")},
                            {QStringLiteral("inputSchema"), QJsonObject{}}});
            tools.append(QJsonObject{
                {QStringLiteral("name"), QStringLiteral("business_error")},
                {QStringLiteral("description"),
                 QStringLiteral("Return a structured business error")},
                {QStringLiteral("inputSchema"), QJsonObject{}}});
            tools.append(
                QJsonObject{{QStringLiteral("name"), QStringLiteral("slow")},
                            {QStringLiteral("description"),
                             QStringLiteral("Delay the response")},
                            {QStringLiteral("inputSchema"), QJsonObject{}}});
            writeResponse(output, id, {{QStringLiteral("tools"), tools}});
        }
        else if (method == QStringLiteral("tools/call"))
        {
            const auto params =
                request.value(QStringLiteral("params")).toObject();
            const auto name = params.value(QStringLiteral("name")).toString();
            if (name == QStringLiteral("error"))
            {
                writeError(output, id, -32000, QStringLiteral("fake failure"));
            }
            else if (name == QStringLiteral("business_error"))
            {
                const auto structured = QJsonObject{
                    {QStringLiteral("ok"), false},
                    {QStringLiteral("error"),
                     QJsonObject{{QStringLiteral("code"),
                                  QStringLiteral("CASE_PATH_EXISTS")},
                                 {QStringLiteral("message"),
                                  QStringLiteral("Case path already exists.")},
                                 {QStringLiteral("recoverable"), true}}}};
                writeResponse(
                    output, id,
                    {{QStringLiteral("content"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("type"), QStringLiteral("text")},
                          {QStringLiteral("text"),
                           QString::fromUtf8(
                               QJsonDocument(structured)
                                   .toJson(QJsonDocument::Compact))}}}},
                     {QStringLiteral("isError"), false},
                     {QStringLiteral("structuredContent"), structured}});
            }
            else if (name == QStringLiteral("slow"))
            {
                QThread::msleep(1'500);
                writeResponse(output, id,
                              {{QStringLiteral("content"), QJsonArray{}}});
            }
            else
            {
                const auto text =
                    QJsonDocument(
                        params.value(QStringLiteral("arguments")).toObject())
                        .toJson(QJsonDocument::Compact);
                QJsonArray content;
                content.append(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), QString::fromUtf8(text)}});
                writeResponse(output, id,
                              {{QStringLiteral("content"), content}});
            }
        }
        else
        {
            writeError(output, id, -32601, QStringLiteral("method not found"));
        }
    }
    return 0;
}
