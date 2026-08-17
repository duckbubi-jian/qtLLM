#include "AgentAction.hpp"

#include <QtTest>

namespace qtllm::tests
{
class AgentActionTest final : public QObject
{
    Q_OBJECT

   private slots:
    void parsesToolCall();
    void parsesTaskPlan();
    void parsesCompletionReview();
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

void AgentActionTest::parsesTaskPlan()
{
    agent::Action action;
    QString errorMessage;
    QVERIFY2(
        agent::parseAction(
            QByteArrayLiteral(
                R"({"action":"task_plan","steps":[{"id":"step-1","description":"Create the case","requires_tool":true}]})"),
            action, errorMessage),
        qPrintable(errorMessage));
    QCOMPARE(action.type, agent::ActionType::TaskPlan);
    QCOMPARE(action.completionSteps.size(), 1);
    QCOMPARE(action.completionSteps.at(0).toObject().value(
                 QStringLiteral("requires_tool")),
             QJsonValue(true));
}

void AgentActionTest::parsesCompletionReview()
{
    agent::Action action;
    QString errorMessage;
    QVERIFY2(
        agent::parseAction(
            QByteArrayLiteral(
                R"({"action":"review_completion","verdict":"complete","steps":[{"id":"step-1","description":"Create the case","requires_tool":true,"status":"satisfied","evidence":[1]}],"detail":"All requested work is complete."})"),
            action, errorMessage),
        qPrintable(errorMessage));
    QCOMPARE(action.type, agent::ActionType::ReviewCompletion);
    QCOMPARE(action.completionVerdict, QStringLiteral("complete"));
    QCOMPARE(action.completionSteps.size(), 1);
    QCOMPARE(action.completionDetail,
             QStringLiteral("All requested work is complete."));
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
    QTest::newRow("duplicate-plan-step") << QByteArrayLiteral(
        R"({"action":"task_plan","steps":[{"id":"same","description":"One","requires_tool":true},{"id":"same","description":"Two","requires_tool":true}]})");
    QTest::newRow("complete-with-pending-step") << QByteArrayLiteral(
        R"({"action":"review_completion","verdict":"complete","steps":[{"id":"step-1","description":"Create","requires_tool":true,"status":"pending","evidence":[]}],"detail":"Done"})");
    QTest::newRow("invalid-evidence-sequence") << QByteArrayLiteral(
        R"({"action":"review_completion","verdict":"complete","steps":[{"id":"step-1","description":"Create","requires_tool":true,"status":"satisfied","evidence":[0]}],"detail":"Done"})");
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
    QVERIFY(grammar.contains("task_plan"));
    QVERIFY(grammar.contains("review_completion"));
    QVERIFY(grammar.contains("final"));
    QVERIFY(grammar.contains("ws ::= [ \\t\\n\\r]{0,8}"));
    QVERIFY(!grammar.contains("ws ::= [ \\t\\n\\r]*"));
}
}  // namespace qtllm::tests

QTEST_APPLESS_MAIN(qtllm::tests::AgentActionTest)

#include "AgentActionTest.moc"
