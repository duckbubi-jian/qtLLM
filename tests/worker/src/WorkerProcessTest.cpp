#include "JsonLineProtocol.hpp"

#include "ProtocolVersion.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
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
}

void WorkerProcessTest::modelLifecycle()
{
    auto modelPath = QString::fromLocal8Bit(qgetenv("QTLLM_TEST_MODEL"));
    if (modelPath.isEmpty())
        QSKIP("Set QTLLM_TEST_MODEL to run the real-model IPC test.");
    if (QFileInfo(modelPath).isDir())
        modelPath = QDir(modelPath).filePath(QStringLiteral("model.gguf"));
    QVERIFY2(QFileInfo::exists(modelPath), qPrintable(modelPath));

    QString errorMessage;
    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("load-1"),
                      QString::fromLatin1(protocol::message_type::loadModel),
                      {{QStringLiteral("modelPath"), modelPath},
                       {QStringLiteral("gpuLayers"), 0}}),
                  errorMessage),
             qPrintable(errorMessage));
    protocol::Message response;
    QVERIFY2(expect(QStringLiteral("load-1"),
                    QString::fromLatin1(protocol::message_type::modelLoaded),
                    response, errorMessage, 120000),
             qPrintable(errorMessage));

    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("generation-cancelled"),
                      QString::fromLatin1(protocol::message_type::generate),
                      generationPayload(
                          QStringLiteral("Please explain local language "
                                         "models in detail."),
                          256)),
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

    QVERIFY2(send(protocol::makeMessage(
                      QStringLiteral("generation-2"),
                      QString::fromLatin1(protocol::message_type::generate),
                      generationPayload(QStringLiteral("Answer only: OK"), 16)),
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
        }
        if (response.type == QLatin1String(protocol::message_type::error))
        {
            QFAIL(qPrintable(
                response.payload.value(QStringLiteral("message")).toString()));
        }
    }
    QVERIFY(generationFinished);
    QVERIFY(!receivedBytes.isEmpty());
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
