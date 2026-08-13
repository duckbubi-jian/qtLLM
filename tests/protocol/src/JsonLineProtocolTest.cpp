#include "JsonLineProtocol.hpp"

#include "ProtocolVersion.hpp"

#include <QJsonObject>
#include <QtTest>

#include <limits>

namespace qtllm::tests
{
class JsonLineProtocolTest final : public QObject
{
    Q_OBJECT

   private slots:
    void roundTripsMessage();
    void rejectsInvalidJson();
    void rejectsMissingFields();
    void rejectsFractionalProtocolVersion();
    void rejectsOutOfRangeProtocolVersion();
};

void JsonLineProtocolTest::roundTripsMessage()
{
    const auto original = protocol::makeMessage(
        QStringLiteral("request-1"), QStringLiteral("generate"),
        {{QStringLiteral("prompt"), QStringLiteral("hello")},
         {QStringLiteral("maxTokens"), 32}});

    const auto line = protocol::encodeLine(original);
    QVERIFY(line.endsWith('\n'));

    protocol::Message decoded;
    QString errorMessage;
    QVERIFY2(protocol::decodeLine(line, decoded, errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(decoded.protocolVersion, protocol::version);
    QCOMPARE(decoded.requestId, original.requestId);
    QCOMPARE(decoded.type, original.type);
    QCOMPARE(decoded.payload, original.payload);
}

void JsonLineProtocolTest::rejectsInvalidJson()
{
    protocol::Message message;
    QString errorMessage;
    QVERIFY(!protocol::decodeLine(QByteArrayLiteral("not-json"), message,
                                  errorMessage));
    QVERIFY(errorMessage.startsWith(QStringLiteral("Invalid JSON object")));
}

void JsonLineProtocolTest::rejectsMissingFields()
{
    protocol::Message message;
    QString errorMessage;
    QVERIFY(!protocol::decodeLine(
        QByteArrayLiteral(R"({"protocolVersion":1,"requestId":"r"})"), message,
        errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("requires")));
}

void JsonLineProtocolTest::rejectsFractionalProtocolVersion()
{
    protocol::Message message;
    QString errorMessage;
    QVERIFY(!protocol::decodeLine(
        QByteArrayLiteral(
            R"({"protocolVersion":1.5,"requestId":"r","type":"hello","payload":{}})"),
        message, errorMessage));
    QCOMPARE(errorMessage,
             QStringLiteral("protocolVersion must be an integer."));
}

void JsonLineProtocolTest::rejectsOutOfRangeProtocolVersion()
{
    protocol::Message message;
    QString errorMessage;
    QVERIFY(!protocol::decodeLine(
        QByteArrayLiteral(
            R"({"protocolVersion":1e100,"requestId":"r","type":"hello","payload":{}})"),
        message, errorMessage));
    QCOMPARE(errorMessage, QStringLiteral("protocolVersion is out of range."));
}
}  // namespace qtllm::tests

QTEST_APPLESS_MAIN(qtllm::tests::JsonLineProtocolTest)

#include "JsonLineProtocolTest.moc"
