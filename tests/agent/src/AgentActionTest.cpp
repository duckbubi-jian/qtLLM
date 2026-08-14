#include "AgentAction.hpp"

#include <QtTest>

namespace qtllm::tests
{
class AgentActionTest final : public QObject
{
    Q_OBJECT

   private slots:
    void parsesToolCall();
    void parsesFinalAnswer();
    void rejectsInvalidActions_data();
    void rejectsInvalidActions();
    void providesGenerationGrammar();
};

void AgentActionTest::parsesToolCall()
{
    agent::Action action;
    QString errorMessage;
    QVERIFY2(
        agent::parseAction(
            QByteArrayLiteral(
                R"({"action":"call_tool","tool":"files.read","arguments":{"path":"README.md"}})"),
            action, errorMessage),
        qPrintable(errorMessage));
    QCOMPARE(action.type, agent::ActionType::CallTool);
    QCOMPARE(action.toolName, QStringLiteral("files.read"));
    QCOMPARE(action.arguments.value(QStringLiteral("path")).toString(),
             QStringLiteral("README.md"));
    QVERIFY(action.content.isEmpty());
}

void AgentActionTest::parsesFinalAnswer()
{
    agent::Action action;
    QString errorMessage;
    QVERIFY2(agent::parseAction(
                 QByteArrayLiteral(R"({"action":"final","content":"Done"})"),
                 action, errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(action.type, agent::ActionType::Final);
    QCOMPARE(action.content, QStringLiteral("Done"));
    QVERIFY(action.toolName.isEmpty());
}

void AgentActionTest::rejectsInvalidActions_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::newRow("not-json") << QByteArrayLiteral("not-json");
    QTest::newRow("unknown") << QByteArrayLiteral(R"({"action":"unknown"})");
    QTest::newRow("missing-tool")
        << QByteArrayLiteral(R"({"action":"call_tool","arguments":{}})");
    QTest::newRow("non-object-arguments") << QByteArrayLiteral(
        R"({"action":"call_tool","tool":"files.read","arguments":[]})");
    QTest::newRow("empty-final")
        << QByteArrayLiteral(R"({"action":"final","content":""})");
    QTest::newRow("extra-property") << QByteArrayLiteral(
        R"({"action":"final","content":"Done","extra":true})");
}

void AgentActionTest::rejectsInvalidActions()
{
    QFETCH(QByteArray, json);
    agent::Action action;
    QString errorMessage;
    QVERIFY(!agent::parseAction(json, action, errorMessage));
    QVERIFY(!errorMessage.isEmpty());
}

void AgentActionTest::providesGenerationGrammar()
{
    const auto grammar = agent::actionGrammar();
    QVERIFY(grammar.contains("root ::="));
    QVERIFY(grammar.contains("call_tool"));
    QVERIFY(grammar.contains("final"));
}
}  // namespace qtllm::tests

QTEST_APPLESS_MAIN(qtllm::tests::AgentActionTest)

#include "AgentActionTest.moc"
