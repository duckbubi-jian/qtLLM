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
    void parsesBlockedResult();
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
                R"({"action":"task_plan","steps":[{"id":"step-1","description":"Create the case","source_ids":["request-1"],"requires_tool":true,"allowed_tools":["files.create"]}],"ordered":true})"),
            action, errorMessage),
        qPrintable(errorMessage));
    QCOMPARE(action.type, agent::ActionType::TaskPlan);
    QVERIFY(action.orderedPlan.has_value());
    QVERIFY(*action.orderedPlan);
    QCOMPARE(action.completionSteps.size(), 1);
    QCOMPARE(action.completionSteps.at(0).toObject().value(
                 QStringLiteral("requires_tool")),
             QJsonValue(true));
    QCOMPARE(action.completionSteps.at(0)
                 .toObject()
                 .value(QStringLiteral("source_ids"))
                 .toArray(),
             QJsonArray{QStringLiteral("request-1")});
}

void AgentActionTest::parsesBlockedResult()
{
    agent::Action action;
    QString errorMessage;
    QVERIFY2(
        agent::parseAction(
            QByteArrayLiteral(
                R"({"action":"blocked","reason":"missing_input","content":"Provide the parent directory."})"),
            action, errorMessage),
        qPrintable(errorMessage));
    QCOMPARE(action.type, agent::ActionType::Blocked);
    QCOMPARE(action.blockReason, QStringLiteral("missing_input"));
    QCOMPARE(action.content, QStringLiteral("Provide the parent directory."));
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
    QTest::newRow("unknown-block-reason") << QByteArrayLiteral(
        R"({"action":"blocked","reason":"gave_up","content":"Cannot continue."})");
    QTest::newRow("empty-block-content") << QByteArrayLiteral(
        R"({"action":"blocked","reason":"missing_input","content":""})");
    QTest::newRow("extra-property") << QByteArrayLiteral(
        R"({"action":"final","content":"Done","extra":true})");
    QTest::newRow("duplicate-plan-step") << QByteArrayLiteral(
        R"({"action":"task_plan","steps":[{"id":"same","description":"One","source_ids":["request-1"],"requires_tool":true,"allowed_tools":["files.read"]},{"id":"same","description":"Two","source_ids":["request-2"],"requires_tool":true,"allowed_tools":["files.read"]}]})");
    QTest::newRow("plan-missing-allowed-tools") << QByteArrayLiteral(
        R"({"action":"task_plan","steps":[{"id":"one","description":"One","source_ids":["request-1"],"requires_tool":true}],"ordered":true})");
    QTest::newRow("plan-missing-source-ids") << QByteArrayLiteral(
        R"({"action":"task_plan","steps":[{"id":"one","description":"One","requires_tool":true,"allowed_tools":["files.read"]}],"ordered":true})");
    QTest::newRow("non-tool-plan-with-tool") << QByteArrayLiteral(
        R"({"action":"task_plan","steps":[{"id":"one","description":"Report","source_ids":["request-1"],"requires_tool":false,"allowed_tools":["files.read"]}],"ordered":true})");
    QTest::newRow("legacy-tool-task-metadata") << QByteArrayLiteral(
        R"({"action":"call_tool","tool":"files.read","arguments":{},"plan_step_id":"one","completes_plan_step":true})");
    QTest::newRow("removed-tool-call-review") << QByteArrayLiteral(
        R"({"action":"review_tool_call","verdict":"allow","detail":"Yes"})");
    QTest::newRow("removed-plan-step-review") << QByteArrayLiteral(
        R"({"action":"review_plan_step","step_id":"one","status":"satisfied","evidence":[1],"detail":"Done"})");
    QTest::newRow("removed-completion-review") << QByteArrayLiteral(
        R"({"action":"review_completion","verdict":"complete","steps":[],"detail":"Done"})");
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
    QVERIFY(grammar.contains("ordered"));
    QVERIFY(!grammar.contains("plan_step_id"));
    QVERIFY(!grammar.contains("completes_plan_step"));
    QVERIFY(!grammar.contains("review_completion"));
    QVERIFY(!grammar.contains("review_tool_call"));
    QVERIFY(!grammar.contains("review_plan_step"));
    QVERIFY(grammar.contains("blocked"));
    QVERIFY(grammar.contains("missing_input"));
    QVERIFY(grammar.contains("final"));
    QVERIFY(grammar.contains("ws ::= [ \\t\\n\\r]{0,8}"));
    QVERIFY(!grammar.contains("ws ::= [ \\t\\n\\r]*"));
}
}  // namespace qtllm::tests

QTEST_APPLESS_MAIN(qtllm::tests::AgentActionTest)

#include "AgentActionTest.moc"
