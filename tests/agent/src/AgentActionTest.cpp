#include "AgentAction.hpp"

#include <QtTest>

namespace qtllm::tests
{
class AgentActionTest final : public QObject
{
    Q_OBJECT

   private slots:
    void parsesToolCall();
    void parsesToolCallPlanStepId();
    void parsesTaskPlan();
    void parsesToolCallReview();
    void parsesPlanStepReview();
    void parsesCompletionReview();
    void parsesBlockedResult();
    void normalizesCompletionReviewStatusAliases();
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
                R"({"action":"call_tool","tool":"files.read","arguments":{"path":"README.md"},"plan_step_id":"","completes_plan_step":false})"),
            action, errorMessage),
        qPrintable(errorMessage));
    QCOMPARE(action.type, agent::ActionType::CallTool);
    QCOMPARE(action.toolName, QStringLiteral("files.read"));
    QVERIFY(action.planStepId.isEmpty());
    QVERIFY(action.completesPlanStep.has_value());
    QVERIFY(!*action.completesPlanStep);
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
                R"({"action":"task_plan","steps":[{"id":"step-1","description":"Create the case","requires_tool":true,"allowed_tools":["files.create"]}],"ordered":true})"),
            action, errorMessage),
        qPrintable(errorMessage));
    QCOMPARE(action.type, agent::ActionType::TaskPlan);
    QVERIFY(action.orderedPlan.has_value());
    QVERIFY(*action.orderedPlan);
    QCOMPARE(action.completionSteps.size(), 1);
    QCOMPARE(action.completionSteps.at(0).toObject().value(
                 QStringLiteral("requires_tool")),
             QJsonValue(true));
}

void AgentActionTest::parsesPlanStepReview()
{
    agent::Action action;
    QString errorMessage;
    QVERIFY2(
        agent::parseAction(
            QByteArrayLiteral(
                R"({"action":"review_plan_step","step_id":"step-1","status":"satisfied","evidence":[1,2],"detail":"The exact requested case was created."})"),
            action, errorMessage),
        qPrintable(errorMessage));
    QCOMPARE(action.type, agent::ActionType::ReviewPlanStep);
    QCOMPARE(action.planStepId, QStringLiteral("step-1"));
    QCOMPARE(action.planStepReviewStatus, QStringLiteral("satisfied"));
    QCOMPARE(action.planStepReviewEvidence, QJsonArray({1, 2}));
}

void AgentActionTest::parsesToolCallReview()
{
    agent::Action action;
    QString errorMessage;
    QVERIFY2(
        agent::parseAction(
            QByteArrayLiteral(
                R"({"action":"review_tool_call","verdict":"reject","detail":"The call belongs to the next step."})"),
            action, errorMessage),
        qPrintable(errorMessage));
    QCOMPARE(action.type, agent::ActionType::ReviewToolCall);
    QCOMPARE(action.toolReviewVerdict, QStringLiteral("reject"));
    QCOMPARE(action.toolReviewDetail,
             QStringLiteral("The call belongs to the next step."));
}

void AgentActionTest::parsesToolCallPlanStepId()
{
    agent::Action action;
    QString errorMessage;
    QVERIFY2(
        agent::parseAction(
            QByteArrayLiteral(
                R"({"action":"call_tool","tool":"files.read","arguments":{"path":"README.md"},"plan_step_id":"step-1","completes_plan_step":true})"),
            action, errorMessage),
        qPrintable(errorMessage));
    QCOMPARE(action.planStepId, QStringLiteral("step-1"));
    QVERIFY(action.completesPlanStep.has_value());
    QVERIFY(*action.completesPlanStep);
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

void AgentActionTest::normalizesCompletionReviewStatusAliases()
{
    const auto parseStatus = [](const QByteArray& json)
    {
        agent::Action action;
        QString errorMessage;
        if (!agent::parseAction(json, action, errorMessage))
            return QStringLiteral("error: ") + errorMessage;
        return action.completionSteps.constFirst()
            .toObject()
            .value(QStringLiteral("status"))
            .toString();
    };

    QCOMPARE(
        parseStatus(QByteArrayLiteral(
            R"({"action":"review_completion","verdict":"complete","steps":[{"id":"step-1","description":"Create","requires_tool":false,"status":"complete","evidence":[]}],"detail":"Done"})")),
        QStringLiteral("satisfied"));
    QCOMPARE(
        parseStatus(QByteArrayLiteral(
            R"({"action":"review_completion","verdict":"continue","steps":[{"id":"step-1","description":"Create","requires_tool":false,"status":"incomplete","evidence":[]}],"detail":"Continue"})")),
        QStringLiteral("pending"));
    QCOMPARE(
        parseStatus(QByteArrayLiteral(
            R"({"action":"review_completion","verdict":"complete","steps":[{"id":"step-1","description":"Create","requires_tool":false,"status":"DONE","evidence":[]}],"detail":"Done"})")),
        QStringLiteral("satisfied"));
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
        R"({"action":"task_plan","steps":[{"id":"same","description":"One","requires_tool":true,"allowed_tools":["files.read"]},{"id":"same","description":"Two","requires_tool":true,"allowed_tools":["files.read"]}]})");
    QTest::newRow("plan-missing-allowed-tools") << QByteArrayLiteral(
        R"({"action":"task_plan","steps":[{"id":"one","description":"One","requires_tool":true}],"ordered":true})");
    QTest::newRow("non-tool-plan-with-tool") << QByteArrayLiteral(
        R"({"action":"task_plan","steps":[{"id":"one","description":"Report","requires_tool":false,"allowed_tools":["files.read"]}],"ordered":true})");
    QTest::newRow("invalid-plan-step-review-status") << QByteArrayLiteral(
        R"({"action":"review_plan_step","step_id":"one","status":"complete","evidence":[1],"detail":"Done"})");
    QTest::newRow("invalid-tool-call-review-verdict") << QByteArrayLiteral(
        R"({"action":"review_tool_call","verdict":"continue","detail":"No"})");
    QTest::newRow("complete-with-pending-step") << QByteArrayLiteral(
        R"({"action":"review_completion","verdict":"complete","steps":[{"id":"step-1","description":"Create","requires_tool":true,"status":"pending","evidence":[]}],"detail":"Done"})");
    QTest::newRow("invalid-evidence-sequence") << QByteArrayLiteral(
        R"({"action":"review_completion","verdict":"complete","steps":[{"id":"step-1","description":"Create","requires_tool":true,"status":"satisfied","evidence":[0]}],"detail":"Done"})");
    QTest::newRow("unknown-review-status") << QByteArrayLiteral(
        R"({"action":"review_completion","verdict":"continue","steps":[{"id":"step-1","description":"Create","requires_tool":true,"status":"failed","evidence":[]}],"detail":"Continue"})");
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
    QVERIFY(grammar.contains("plan_step_id"));
    QVERIFY(grammar.contains("completes_plan_step"));
    QVERIFY(grammar.contains(QByteArrayLiteral(
        "object ws \",\" ws \"\\\"plan_step_id\\\"\" ws \":\" ws string")));
    QVERIFY(!grammar.contains(
        QByteArrayLiteral("object (ws \",\" ws \"\\\"plan_step_id\\\"\"")));
    QVERIFY(grammar.contains("review_completion"));
    QVERIFY(grammar.contains("review_tool_call"));
    QVERIFY(grammar.contains("review_plan_step"));
    QVERIFY(grammar.contains("blocked"));
    QVERIFY(grammar.contains("missing_input"));
    QVERIFY(grammar.contains("review-status ::="));
    QVERIFY(grammar.contains("\"\\\"satisfied\\\"\""));
    QVERIFY(grammar.contains("final"));
    QVERIFY(grammar.contains("ws ::= [ \\t\\n\\r]{0,8}"));
    QVERIFY(!grammar.contains("ws ::= [ \\t\\n\\r]*"));
}
}  // namespace qtllm::tests

QTEST_APPLESS_MAIN(qtllm::tests::AgentActionTest)

#include "AgentActionTest.moc"
