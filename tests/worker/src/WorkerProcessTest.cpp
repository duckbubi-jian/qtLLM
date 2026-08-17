#include "JsonLineProtocol.hpp"

#include "ProtocolVersion.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QtTest>

namespace qtllm::tests
{
class WorkerProcessTest final : public QObject
{
    Q_OBJECT

   private slots:
    void initTestCase();
    void hello();
    void rejectsInvalidJson();
    void rejectsProtocolMismatch();
    void rejectsGenerationBeforeModelLoad();
    void rejectsInvalidMessageHistory();
    void rejectsInvalidResponseMode();
    void rejectsInvalidDevicePlacement();
    void listsComputeDevices();
    void reportsIdleStatus();
    void modelLifecycle();
    void cleanupTestCase();

   private:
    bool send(const protocol::Message& message, QString& errorMessage);
    bool read(protocol::Message& message, QString& errorMessage,
              int timeoutMilliseconds = 5000);
    bool expect(const QString& requestId, const QString& type,
                protocol::Message& message, QString& errorMessage,
                int timeoutMilliseconds = 5000);
    bool expectError(const QString& requestId, const QString& code,
                     QString& errorMessage);
    QJsonObject generationPayload(const QString& prompt, int maxTokens) const;

    QProcess worker_;
    QByteArray outputBuffer_;
    QByteArray diagnostics_;
};

void WorkerProcessTest::initTestCase()
{
    const auto workerPath =
        QString::fromLocal8Bit(qgetenv("QTLLM_TEST_WORKER"));
    QVERIFY2(!workerPath.isEmpty(),
             "The QTLLM_TEST_WORKER environment variable is required.");
    QVERIFY2(QFileInfo::exists(workerPath), qPrintable(workerPath));

    worker_.setProcessChannelMode(QProcess::SeparateChannels);
    worker_.setProgram(workerPath);
    worker_.setArguments({QStringLiteral("--ipc")});
    worker_.start();
    QVERIFY2(worker_.waitForStarted(5000), qPrintable(worker_.errorString()));
}

void WorkerProcessTest::hello()
{
    QString errorMessage;
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("hello-1"),
                      QString::fromLatin1(protocol::message_type::hello)),
                  errorMessage),
             qPrintable(errorMessage));

    protocol::Message response;
    QVERIFY2(expect(QStringLiteral("hello-1"),
                    QString::fromLatin1(protocol::message_type::hello),
                    response, errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(response.payload.value(QStringLiteral("protocolVersion")).toInt(),
             protocol::version);
    QVERIFY(!response.payload.value(QStringLiteral("workerVersion"))
                 .toString()
                 .isEmpty());
    const auto capabilities =
        response.payload.value(QStringLiteral("capabilities")).toObject();
    QCOMPARE(
        capabilities.value(QStringLiteral("structuredGeneration")).toBool(),
        true);
    QCOMPARE(capabilities.value(QStringLiteral("grammar")).toBool(), true);
    QCOMPARE(capabilities.value(QStringLiteral("gpuDeviceDiscovery")).toBool(),
             true);
    QCOMPARE(capabilities.value(QStringLiteral("multiGpuLayerSplit")).toBool(),
             true);
}

void WorkerProcessTest::rejectsInvalidJson()
{
    worker_.write("not-json\n");
    QVERIFY(worker_.waitForBytesWritten(5000));

    protocol::Message response;
    QString errorMessage;
    QVERIFY2(expect(QStringLiteral("invalid-message"),
                    QString::fromLatin1(protocol::message_type::error),
                    response, errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(response.payload.value(QStringLiteral("code")).toString(),
             QStringLiteral("invalid_message"));
}

void WorkerProcessTest::rejectsProtocolMismatch()
{
    auto request = protocol::makeMessage(
        QStringLiteral("version-1"),
        QString::fromLatin1(protocol::message_type::hello));
    request.protocolVersion = protocol::version + 1;

    QString errorMessage;
    QVERIFY2(send(request, errorMessage), qPrintable(errorMessage));
    QVERIFY2(expectError(QStringLiteral("version-1"),
                         QStringLiteral("protocol_mismatch"), errorMessage),
             qPrintable(errorMessage));
}

void WorkerProcessTest::rejectsGenerationBeforeModelLoad()
{
    QString errorMessage;
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("generate-unloaded"),
                      QString::fromLatin1(protocol::message_type::generate),
                      generationPayload(QStringLiteral("hello"), 8)),
                  errorMessage),
             qPrintable(errorMessage));
    QVERIFY2(expectError(QStringLiteral("generate-unloaded"),
                         QStringLiteral("model_not_loaded"), errorMessage),
             qPrintable(errorMessage));
}

void WorkerProcessTest::rejectsInvalidMessageHistory()
{
    QString errorMessage;
    auto payload = generationPayload({}, 8);
    payload.remove(QStringLiteral("prompt"));
    payload.remove(QStringLiteral("systemPrompt"));
    payload.insert(QStringLiteral("messages"),
                   QJsonArray{QJsonObject{
                       {QStringLiteral("role"), QStringLiteral("assistant")},
                       {QStringLiteral("content"), QStringLiteral("hello")}}});
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("invalid-history"),
                      QString::fromLatin1(protocol::message_type::generate),
                      payload),
                  errorMessage),
             qPrintable(errorMessage));
    QVERIFY2(expectError(QStringLiteral("invalid-history"),
                         QStringLiteral("invalid_payload"), errorMessage),
             qPrintable(errorMessage));
}

void WorkerProcessTest::rejectsInvalidResponseMode()
{
    auto payload = generationPayload(QStringLiteral("hello"), 8);
    payload.insert(QStringLiteral("responseMode"), QStringLiteral("invalid"));

    QString errorMessage;
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("invalid-response-mode"),
                      QString::fromLatin1(protocol::message_type::generate),
                      payload),
                  errorMessage),
             qPrintable(errorMessage));
    QVERIFY2(expectError(QStringLiteral("invalid-response-mode"),
                         QStringLiteral("invalid_payload"), errorMessage),
             qPrintable(errorMessage));
}

void WorkerProcessTest::rejectsInvalidDevicePlacement()
{
    QString errorMessage;
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("invalid-placement"),
                      QString::fromLatin1(protocol::message_type::loadModel),
                      {{QStringLiteral("modelPath"),
                        QStringLiteral("C:/invalid.gguf")},
                       {QStringLiteral("placement"),
                        QJsonObject{
                            {QStringLiteral("mode"), QStringLiteral("single")},
                            {QStringLiteral("deviceIds"), QJsonArray{}},
                            {QStringLiteral("weights"), QJsonArray{}}}}}),
                  errorMessage),
             qPrintable(errorMessage));
    QVERIFY2(expectError(QStringLiteral("invalid-placement"),
                         QStringLiteral("invalid_payload"), errorMessage),
             qPrintable(errorMessage));
}

void WorkerProcessTest::listsComputeDevices()
{
    QString errorMessage;
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("devices-1"),
                      QString::fromLatin1(protocol::message_type::listDevices)),
                  errorMessage),
             qPrintable(errorMessage));

    protocol::Message response;
    QVERIFY2(expect(QStringLiteral("devices-1"),
                    QString::fromLatin1(protocol::message_type::devices),
                    response, errorMessage),
             qPrintable(errorMessage));
    QVERIFY(response.payload.value(QStringLiteral("devices")).isArray());
    const auto devices =
        response.payload.value(QStringLiteral("devices")).toArray();
    for (const auto& value : devices)
    {
        QVERIFY(value.isObject());
        const auto device = value.toObject();
        QVERIFY(!device.value(QStringLiteral("id")).toString().isEmpty());
        QVERIFY(
            !device.value(QStringLiteral("backendName")).toString().isEmpty());
        QVERIFY(device.value(QStringLiteral("freeMemoryBytes")).toDouble() <=
                device.value(QStringLiteral("totalMemoryBytes")).toDouble());
    }
}

void WorkerProcessTest::reportsIdleStatus()
{
    QString errorMessage;
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("status-1"),
                      QString::fromLatin1(protocol::message_type::getStatus)),
                  errorMessage),
             qPrintable(errorMessage));

    protocol::Message response;
    QVERIFY2(expect(QStringLiteral("status-1"),
                    QString::fromLatin1(protocol::message_type::status),
                    response, errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(response.payload.value(QStringLiteral("modelLoaded")).toBool(),
             false);
    QCOMPARE(response.payload.value(QStringLiteral("generating")).toBool(),
             false);
    QCOMPARE(response.payload.value(QStringLiteral("splitMode")).toString(),
             QStringLiteral("cpu"));
    QVERIFY(response.payload.value(QStringLiteral("devices")).isArray());
}

void WorkerProcessTest::modelLifecycle()
{
    auto modelPath = QString::fromLocal8Bit(qgetenv("QTLLM_TEST_MODEL"));
    if (modelPath.isEmpty())
        QSKIP("Set QTLLM_TEST_MODEL to run the real-model IPC test.");
    if (QFileInfo(modelPath).isDir())
        modelPath = QDir(modelPath).filePath(QStringLiteral("model.gguf"));
    QVERIFY2(QFileInfo::exists(modelPath), qPrintable(modelPath));

    bool validGpuLayers = false;
    const auto gpuLayers =
        qEnvironmentVariableIntValue("QTLLM_TEST_GPU_LAYERS", &validGpuLayers);
    const auto requestedGpuLayers = validGpuLayers ? gpuLayers : 0;
    QVERIFY2(requestedGpuLayers >= -1 && requestedGpuLayers <= 10'000,
             "QTLLM_TEST_GPU_LAYERS must be between -1 and 10000.");

    QString errorMessage;
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("load-1"),
                      QString::fromLatin1(protocol::message_type::loadModel),
                      {{QStringLiteral("modelPath"), modelPath},
                       {QStringLiteral("gpuLayers"), requestedGpuLayers}}),
                  errorMessage),
             qPrintable(errorMessage));
    protocol::Message response;
    QVERIFY2(expect(QStringLiteral("load-1"),
                    QString::fromLatin1(protocol::message_type::modelLoaded),
                    response, errorMessage, 120000),
             qPrintable(errorMessage));
    QVERIFY(
        !response.payload.value(QStringLiteral("device")).toString().isEmpty());
    QVERIFY(response.payload.value(QStringLiteral("devices")).isArray());
    const auto activeDevices =
        response.payload.value(QStringLiteral("devices")).toArray();
    const auto splitMode =
        response.payload.value(QStringLiteral("splitMode")).toString();
    QVERIFY(splitMode == QLatin1String("cpu") ||
            splitMode == QLatin1String("single") ||
            splitMode == QLatin1String("layer"));
    bool validExpectedGpuCount = false;
    const auto expectedGpuCount = qEnvironmentVariableIntValue(
        "QTLLM_TEST_EXPECTED_GPU_COUNT", &validExpectedGpuCount);
    if (validExpectedGpuCount)
    {
        QVERIFY2(expectedGpuCount >= 0,
                 "QTLLM_TEST_EXPECTED_GPU_COUNT must not be negative.");
        QCOMPARE(activeDevices.size(), expectedGpuCount);
        QCOMPARE(splitMode, expectedGpuCount > 1    ? QStringLiteral("layer")
                            : expectedGpuCount == 1 ? QStringLiteral("single")
                                                    : QStringLiteral("cpu"));
    }
    const auto expectedDevice =
        QString::fromLocal8Bit(qgetenv("QTLLM_TEST_DEVICE_CONTAINS"));
    if (!expectedDevice.isEmpty())
    {
        QVERIFY2(
            response.payload.value(QStringLiteral("device"))
                .toString()
                .contains(expectedDevice, Qt::CaseInsensitive),
            qPrintable(
                response.payload.value(QStringLiteral("device")).toString()));
    }

    auto structuredPayload = generationPayload(QStringLiteral("hello"), 256);
    structuredPayload.insert(QStringLiteral("responseMode"),
                             QStringLiteral("agent_action"));
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("generation-cancelled"),
                      QString::fromLatin1(protocol::message_type::generate),
                      structuredPayload),
                  errorMessage),
             qPrintable(errorMessage));
    QVERIFY2(
        expect(QStringLiteral("generation-cancelled"),
               QString::fromLatin1(protocol::message_type::generationStarted),
               response, errorMessage, 120000),
        qPrintable(errorMessage));
    QVERIFY2(expect(QStringLiteral("generation-cancelled"),
                    QString::fromLatin1(protocol::message_type::token),
                    response, errorMessage, 120000),
             qPrintable(errorMessage));
    QVERIFY(
        !QByteArray::fromBase64(response.payload.value(QStringLiteral("data"))
                                    .toString()
                                    .toLatin1())
             .isEmpty());

    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("cancel-1"),
                      QString::fromLatin1(protocol::message_type::cancel),
                      {{QStringLiteral("targetRequestId"),
                        QStringLiteral("generation-cancelled")}}),
                  errorMessage),
             qPrintable(errorMessage));

    auto cancelAccepted = false;
    auto generationFinished = false;
    QElapsedTimer cancellationTimer;
    cancellationTimer.start();
    while ((!cancelAccepted || !generationFinished) &&
           cancellationTimer.elapsed() < 120000)
    {
        QVERIFY2(read(response, errorMessage, 120000),
                 qPrintable(errorMessage));
        if (response.requestId == QStringLiteral("cancel-1") &&
            response.type == QLatin1String(protocol::message_type::status))
        {
            cancelAccepted =
                response.payload.value(QStringLiteral("cancelAccepted"))
                    .toBool();
        }
        if (response.requestId == QStringLiteral("generation-cancelled") &&
            response.type ==
                QLatin1String(protocol::message_type::generationFinished))
        {
            generationFinished = true;
            QCOMPARE(
                response.payload.value(QStringLiteral("cancelled")).toBool(),
                true);
        }
    }
    QVERIFY(cancelAccepted);
    QVERIFY(generationFinished);

    QVERIFY2(
        send(protocol::makeMessage(
                 QStringLiteral("generation-2"),
                 QString::fromLatin1(protocol::message_type::generate),
                 [&]
                 {
                     auto payload = generationPayload({}, 16);
                     payload.remove(QStringLiteral("prompt"));
                     payload.remove(QStringLiteral("systemPrompt"));
                     payload.insert(
                         QStringLiteral("messages"),
                         QJsonArray{
                             QJsonObject{{QStringLiteral("role"),
                                          QStringLiteral("system")},
                                         {QStringLiteral("content"),
                                          QStringLiteral(
                                              "You are a concise assistant.")}},
                             QJsonObject{
                                 {QStringLiteral("role"),
                                  QStringLiteral("user")},
                                 {QStringLiteral("content"),
                                  QStringLiteral("Remember number 42.")}},
                             QJsonObject{
                                 {QStringLiteral("role"),
                                  QStringLiteral("assistant")},
                                 {QStringLiteral("content"),
                                  QStringLiteral("I will remember 42.")}},
                             QJsonObject{
                                 {QStringLiteral("role"),
                                  QStringLiteral("user")},
                                 {QStringLiteral("content"),
                                  QStringLiteral(
                                      "Answer only with the number.")}}});
                     return payload;
                 }()),
             errorMessage),
        qPrintable(errorMessage));

    auto receivedBytes = QByteArray{};
    generationFinished = false;
    QElapsedTimer secondGenerationTimer;
    secondGenerationTimer.start();
    while (!generationFinished && secondGenerationTimer.elapsed() < 120000)
    {
        QVERIFY2(read(response, errorMessage, 120000),
                 qPrintable(errorMessage));
        if (response.requestId != QStringLiteral("generation-2")) continue;
        if (response.type == QLatin1String(protocol::message_type::token))
        {
            receivedBytes += QByteArray::fromBase64(
                response.payload.value(QStringLiteral("data"))
                    .toString()
                    .toLatin1());
        }
        if (response.type ==
            QLatin1String(protocol::message_type::generationFinished))
        {
            generationFinished = true;
            QCOMPARE(
                response.payload.value(QStringLiteral("cancelled")).toBool(),
                false);
            QCOMPARE(response.payload.value(QStringLiteral("discardedMessages"))
                         .toInt(),
                     0);
        }
        if (response.type == QLatin1String(protocol::message_type::error))
        {
            QFAIL(qPrintable(
                response.payload.value(QStringLiteral("message")).toString()));
        }
    }
    QVERIFY(generationFinished);
    QVERIFY(!receivedBytes.isEmpty());

    auto trimmedHistoryPayload = generationPayload({}, 8);
    trimmedHistoryPayload.remove(QStringLiteral("prompt"));
    trimmedHistoryPayload.remove(QStringLiteral("systemPrompt"));
    trimmedHistoryPayload.insert(QStringLiteral("contextSize"), 256);
    trimmedHistoryPayload.insert(
        QStringLiteral("messages"),
        QJsonArray{
            QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                        {QStringLiteral("content"),
                         QStringLiteral("You are a concise assistant.")}},
            QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                        {QStringLiteral("content"),
                         QStringLiteral("old context ").repeated(300)}},
            QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                        {QStringLiteral("content"),
                         QStringLiteral("old answer ").repeated(300)}},
            QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                        {QStringLiteral("content"),
                         QStringLiteral("Answer only: OK")}}});
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("generation-trimmed"),
                      QString::fromLatin1(protocol::message_type::generate),
                      trimmedHistoryPayload),
                  errorMessage),
             qPrintable(errorMessage));

    generationFinished = false;
    while (!generationFinished)
    {
        QVERIFY2(read(response, errorMessage, 120000),
                 qPrintable(errorMessage));
        if (response.requestId != QStringLiteral("generation-trimmed"))
            continue;
        if (response.type ==
            QLatin1String(protocol::message_type::generationFinished))
        {
            generationFinished = true;
            QCOMPARE(
                response.payload.value(QStringLiteral("cancelled")).toBool(),
                false);
            QCOMPARE(response.payload.value(QStringLiteral("discardedMessages"))
                         .toInt(),
                     2);
        }
        if (response.type == QLatin1String(protocol::message_type::error))
        {
            QFAIL(qPrintable(
                response.payload.value(QStringLiteral("message")).toString()));
        }
    }

    auto oversizedInputPayload = generationPayload({}, 64);
    oversizedInputPayload.remove(QStringLiteral("prompt"));
    oversizedInputPayload.remove(QStringLiteral("systemPrompt"));
    oversizedInputPayload.insert(QStringLiteral("contextSize"), 256);
    oversizedInputPayload.insert(
        QStringLiteral("messages"),
        QJsonArray{
            QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                        {QStringLiteral("content"),
                         QStringLiteral("You are a concise assistant.")}},
            QJsonObject{
                {QStringLiteral("role"), QStringLiteral("user")},
                {QStringLiteral("content"),
                 QStringLiteral("oversized latest input ").repeated(1000)}}});
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("generation-oversized"),
                      QString::fromLatin1(protocol::message_type::generate),
                      oversizedInputPayload),
                  errorMessage),
             qPrintable(errorMessage));
    QVERIFY2(
        expect(QStringLiteral("generation-oversized"),
               QString::fromLatin1(protocol::message_type::generationStarted),
               response, errorMessage, 120000),
        qPrintable(errorMessage));
    QVERIFY2(expect(QStringLiteral("generation-oversized"),
                    QString::fromLatin1(protocol::message_type::error),
                    response, errorMessage, 120000),
             qPrintable(errorMessage));
    QCOMPARE(response.payload.value(QStringLiteral("code")).toString(),
             QStringLiteral("generation_failed"));
    const auto overflowMessage =
        response.payload.value(QStringLiteral("message")).toString();
    QVERIFY2(overflowMessage.contains(QStringLiteral("without truncation")),
             qPrintable(overflowMessage));
    QVERIFY2(overflowMessage.contains(QStringLiteral("64 tokens")),
             qPrintable(overflowMessage));
    QVERIFY2(overflowMessage.contains(QStringLiteral("256 tokens")),
             qPrintable(overflowMessage));
}

void WorkerProcessTest::cleanupTestCase()
{
    worker_.closeWriteChannel();
    if (!worker_.waitForFinished(5000))
    {
        worker_.kill();
        worker_.waitForFinished(5000);
    }
}

bool WorkerProcessTest::send(const protocol::Message& message,
                             QString& errorMessage)
{
    if (worker_.state() != QProcess::Running)
    {
        errorMessage = QStringLiteral("Worker is not running: %1")
                           .arg(QString::fromLocal8Bit(diagnostics_));
        return false;
    }
    if (worker_.write(protocol::encodeLine(message)) < 0 ||
        !worker_.waitForBytesWritten(5000))
    {
        errorMessage = worker_.errorString();
        return false;
    }
    return true;
}

bool WorkerProcessTest::read(protocol::Message& message, QString& errorMessage,
                             int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (true)
    {
        const auto newline = outputBuffer_.indexOf('\n');
        if (newline >= 0)
        {
            const auto line = outputBuffer_.left(newline);
            outputBuffer_.remove(0, newline + 1);
            return protocol::decodeLine(line, message, errorMessage);
        }

        const auto remaining = timeoutMilliseconds - timer.elapsed();
        if (remaining <= 0 ||
            !worker_.waitForReadyRead(static_cast<int>(remaining)))
        {
            diagnostics_ += worker_.readAllStandardError();
            errorMessage = QStringLiteral("Timed out waiting for worker: %1")
                               .arg(QString::fromLocal8Bit(diagnostics_));
            return false;
        }
        outputBuffer_ += worker_.readAllStandardOutput();
        diagnostics_ += worker_.readAllStandardError();
    }
}

bool WorkerProcessTest::expect(const QString& requestId, const QString& type,
                               protocol::Message& message,
                               QString& errorMessage, int timeoutMilliseconds)
{
    if (!read(message, errorMessage, timeoutMilliseconds)) return false;
    if (message.requestId != requestId || message.type != type)
    {
        errorMessage =
            QStringLiteral("Expected %1/%2 but received %3/%4.")
                .arg(requestId, type, message.requestId, message.type);
        return false;
    }
    return true;
}

bool WorkerProcessTest::expectError(const QString& requestId,
                                    const QString& code, QString& errorMessage)
{
    protocol::Message response;
    if (!expect(requestId, QString::fromLatin1(protocol::message_type::error),
                response, errorMessage))
        return false;
    const auto actualCode =
        response.payload.value(QStringLiteral("code")).toString();
    if (actualCode != code)
    {
        errorMessage = QStringLiteral("Expected error %1 but received %2.")
                           .arg(code, actualCode);
        return false;
    }
    return true;
}

QJsonObject WorkerProcessTest::generationPayload(const QString& prompt,
                                                 int maxTokens) const
{
    return {{QStringLiteral("prompt"), prompt},
            {QStringLiteral("systemPrompt"),
             QStringLiteral("You are a concise assistant.")},
            {QStringLiteral("contextSize"), 2048},
            {QStringLiteral("maxTokens"), maxTokens},
            {QStringLiteral("temperature"), 0.1},
            {QStringLiteral("topP"), 0.9},
            {QStringLiteral("topK"), 40},
            {QStringLiteral("repeatPenalty"), 1.05}};
}
}  // namespace qtllm::tests

QTEST_APPLESS_MAIN(qtllm::tests::WorkerProcessTest)

#include "WorkerProcessTest.moc"
