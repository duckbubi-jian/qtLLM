#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

namespace
{
QByteArray outputLineEnding = QByteArrayLiteral("\n");
bool fragmentOutput = false;

void writeMessage(QFile& output, const QJsonObject& object)
{
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (fragmentOutput && data.size() > 1)
    {
        const auto split = data.size() / 2;
        output.write(data.first(split));
        output.flush();
        QThread::msleep(5);
        output.write(data.sliced(split));
    }
    else
    {
        output.write(data);
    }
    output.write(outputLineEnding);
    output.flush();
}

void writeResponse(QFile& output, const QJsonValue& id,
                   const QJsonObject& result)
{
    const auto response =
        QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("result"), result}};
    writeMessage(output, response);
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
    writeMessage(output, response);
}

void writeNotification(QFile& output, const QString& method,
                       const QJsonObject& params)
{
    const auto notification =
        QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("method"), method},
                    {QStringLiteral("params"), params}};
    writeMessage(output, notification);
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
    const auto requestRoots =
        arguments.contains(QStringLiteral("--request-roots"));
    const auto requestPing =
        arguments.contains(QStringLiteral("--request-ping"));
    outputLineEnding = arguments.contains(QStringLiteral("--crlf"))
                           ? QByteArrayLiteral("\r\n")
                           : QByteArrayLiteral("\n");
    fragmentOutput = arguments.contains(QStringLiteral("--fragment-output"));
    auto exitCode = exitImmediately ? 0 : -1;
    const auto exitCodePrefix = QStringLiteral("--exit-code=");
    auto stderrLines = 0;
    const auto stderrPrefix = QStringLiteral("--stderr-lines=");
    QString protocolVersion;
    const auto protocolVersionPrefix = QStringLiteral("--protocol-version=");
    for (const auto& argument : arguments)
    {
        if (argument.startsWith(protocolVersionPrefix))
            protocolVersion = argument.mid(protocolVersionPrefix.size());
        else if (argument.startsWith(exitCodePrefix))
            exitCode = argument.mid(exitCodePrefix.size()).toInt();
        else if (argument.startsWith(stderrPrefix))
            stderrLines = argument.mid(stderrPrefix.size()).toInt();
    }

    QFile input;
    QFile output;
    QFile error;
    if (!input.open(stdin, QIODevice::ReadOnly) ||
        !output.open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered) ||
        !error.open(stderr, QIODevice::WriteOnly | QIODevice::Unbuffered))
        return 2;
    if (exitCode >= 0) return exitCode;
    for (const auto& argument : arguments)
    {
        if (argument.startsWith(QStringLiteral("--require-cwd=")) &&
            QDir::cleanPath(argument.mid(14)) !=
                QDir::cleanPath(QDir::currentPath()))
            return 4;
        if (argument.startsWith(QStringLiteral("--require-env=")))
        {
            const auto requirement = argument.mid(14);
            const auto separator = requirement.indexOf(QLatin1Char('='));
            const auto variableName = requirement.left(separator).toUtf8();
            if (separator <= 0 ||
                qEnvironmentVariable(variableName.constData()) !=
                    requirement.mid(separator + 1))
                return 5;
        }
    }
    for (auto index = 0; index < stderrLines; ++index)
        error.write(QByteArrayLiteral("fixture diagnostic line\n"));
    error.flush();
    if (arguments.contains(QStringLiteral("--startup-noise")))
    {
        output.write(QByteArrayLiteral("starting third-party server") +
                     outputLineEnding);
        output.flush();
    }
    if (arguments.contains(QStringLiteral("--oversized-stdout")))
    {
        output.write(QByteArray(1'048'577, 'x'));
        output.flush();
    }

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
        if (method.isEmpty() &&
            id.toString() == QLatin1String("server-roots-1"))
        {
            writeNotification(
                output, QStringLiteral("test/roots_received"),
                request.value(QStringLiteral("result")).toObject());
            continue;
        }
        if (method.isEmpty() && id.toString() == QLatin1String("server-ping-1"))
        {
            writeNotification(
                output, QStringLiteral("test/ping_received"),
                request.value(QStringLiteral("result")).toObject());
            continue;
        }
        if (method == QStringLiteral("notifications/cancelled"))
        {
            writeNotification(
                output, QStringLiteral("test/cancelled_seen"),
                request.value(QStringLiteral("params")).toObject());
            continue;
        }
        if (method == QLatin1String("notifications/initialized"))
        {
            if (requestRoots)
            {
                const auto rootRequest = QJsonObject{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), QStringLiteral("server-roots-1")},
                    {QStringLiteral("method"), QStringLiteral("roots/list")},
                    {QStringLiteral("params"), QJsonObject{}}};
                output.write(
                    QJsonDocument(rootRequest).toJson(QJsonDocument::Compact) +
                    '\n');
                output.flush();
            }
            if (requestPing)
            {
                const auto pingRequest = QJsonObject{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), QStringLiteral("server-ping-1")},
                    {QStringLiteral("method"), QStringLiteral("ping")},
                    {QStringLiteral("params"), QJsonObject{}}};
                output.write(
                    QJsonDocument(pingRequest).toJson(QJsonDocument::Compact) +
                    '\n');
                output.flush();
            }
            continue;
        }
        if (method.startsWith(QStringLiteral("notifications/"))) continue;
        if (method == QStringLiteral("initialize"))
        {
            const auto requestedVersion =
                request.value(QStringLiteral("params"))
                    .toObject()
                    .value(QStringLiteral("protocolVersion"))
                    .toString();
            const auto negotiatedVersion =
                protocolVersion.isEmpty() ? requestedVersion : protocolVersion;
            QJsonObject capabilities{
                {QStringLiteral("resources"),
                 QJsonObject{{QStringLiteral("subscribe"), true},
                             {QStringLiteral("listChanged"), true}}},
                {QStringLiteral("prompts"),
                 QJsonObject{{QStringLiteral("listChanged"), true}}},
                {QStringLiteral("logging"), QJsonObject{}},
                {QStringLiteral("completions"), QJsonObject{}}};
            if (!noToolsCapability)
                capabilities.insert(QStringLiteral("tools"), QJsonObject{});
            writeResponse(
                output, id,
                {{QStringLiteral("protocolVersion"), negotiatedVersion},
                 {QStringLiteral("capabilities"), capabilities},
                 {QStringLiteral("serverInfo"),
                  QJsonObject{
                      {QStringLiteral("name"), QStringLiteral("fake")},
                      {QStringLiteral("version"), QStringLiteral("1")}}},
                 {QStringLiteral("instructions"),
                  QStringLiteral("Use fake tools carefully.")}});
        }
        else if (method == QStringLiteral("ping"))
        {
            writeResponse(output, id, {});
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
        else if (method == QStringLiteral("resources/list"))
        {
            writeResponse(
                output, id,
                {{QStringLiteral("resources"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("uri"),
                       QStringLiteral("test://resource/readme")},
                      {QStringLiteral("name"), QStringLiteral("Readme")},
                      {QStringLiteral("description"),
                       QStringLiteral("Fake resource")},
                      {QStringLiteral("mimeType"),
                       QStringLiteral("text/plain")}}}}});
        }
        else if (method == QStringLiteral("resources/templates/list"))
        {
            writeResponse(output, id,
                          {{QStringLiteral("resourceTemplates"),
                            QJsonArray{QJsonObject{
                                {QStringLiteral("uriTemplate"),
                                 QStringLiteral("test://resource/{name}")},
                                {QStringLiteral("name"),
                                 QStringLiteral("Named resource")},
                                {QStringLiteral("mimeType"),
                                 QStringLiteral("text/plain")}}}}});
        }
        else if (method == QStringLiteral("resources/read"))
        {
            const auto uri = request.value(QStringLiteral("params"))
                                 .toObject()
                                 .value(QStringLiteral("uri"))
                                 .toString();
            writeResponse(output, id,
                          {{QStringLiteral("contents"),
                            QJsonArray{QJsonObject{
                                {QStringLiteral("uri"), uri},
                                {QStringLiteral("mimeType"),
                                 QStringLiteral("text/plain")},
                                {QStringLiteral("text"),
                                 QStringLiteral("Fake resource content")}}}}});
        }
        else if (method == QStringLiteral("resources/subscribe") ||
                 method == QStringLiteral("resources/unsubscribe"))
        {
            writeResponse(output, id, {});
        }
        else if (method == QStringLiteral("prompts/list"))
        {
            writeResponse(
                output, id,
                {{QStringLiteral("prompts"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("name"), QStringLiteral("summarize")},
                      {QStringLiteral("description"),
                       QStringLiteral("Summarize a topic")},
                      {QStringLiteral("arguments"),
                       QJsonArray{QJsonObject{
                           {QStringLiteral("name"), QStringLiteral("topic")},
                           {QStringLiteral("required"), true}}}}}}}});
        }
        else if (method == QStringLiteral("prompts/get"))
        {
            const auto topic = request.value(QStringLiteral("params"))
                                   .toObject()
                                   .value(QStringLiteral("arguments"))
                                   .toObject()
                                   .value(QStringLiteral("topic"))
                                   .toString();
            writeResponse(
                output, id,
                {{QStringLiteral("description"),
                  QStringLiteral("Generated fake prompt")},
                 {QStringLiteral("messages"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("role"), QStringLiteral("user")},
                      {QStringLiteral("content"),
                       QJsonObject{
                           {QStringLiteral("type"), QStringLiteral("text")},
                           {QStringLiteral("text"),
                            QStringLiteral("Summarize %1").arg(topic)}}}}}}});
        }
        else if (method == QStringLiteral("completion/complete"))
        {
            const auto params =
                request.value(QStringLiteral("params")).toObject();
            const auto partial = params.value(QStringLiteral("argument"))
                                     .toObject()
                                     .value(QStringLiteral("value"))
                                     .toString();
            writeResponse(
                output, id,
                {{QStringLiteral("completion"),
                  QJsonObject{{QStringLiteral("values"),
                               QJsonArray{partial + QStringLiteral("-one"),
                                          partial + QStringLiteral("-two")}},
                              {QStringLiteral("total"), 2},
                              {QStringLiteral("hasMore"), false}}}});
        }
        else if (method == QStringLiteral("logging/setLevel"))
        {
            writeResponse(output, id, {});
        }
        else
        {
            writeError(output, id, -32601, QStringLiteral("method not found"));
        }
    }
    return 0;
}
