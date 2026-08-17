#include "AgentController.hpp"
#include "ToolCatalogBuilder.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>

namespace qtllm::tests
{
class AgentControllerTest final : public QObject
{
    Q_OBJECT

   private slots:
    void completesMultiStepToolRun();
    void simpleToolRunCompletesWithoutReview();
    void simpleTaskRecoversFromFailureWithoutStructuredReview();
    void waitsForApprovalAndHonorsRejection();
    void repairsOnlyOneInvalidAction();
    void cancelsAndIgnoresLateResponses();
    void keepsConversationHistoryAndClearsIt();
    void emptyToolPromptForbidsToolCalls();
    void casualPromptPrefersFinalWithoutTools();
    void includesRuntimeContextInPrompt();
    void toolCatalogIsStableAndValid();
    void toolCatalogOmitsWholeDefinitions();
    void rejectsUnchangedRetryAfterToolError();
    void rejectsRepeatedSuccessfulToolCall();
    void allowsRepeatedPollingUntilTerminalStatus();
    void cancelsPendingStatusPoll();
    void rejectsAlternatingCompletedToolCycle();
    void reviewsPrematureFinalBeforeAnyToolCall();
    void reviewsCompletionAfterEachEvidenceRevision();
    void rejectsRepeatedFinalDuringCompletionReview();
    void completionReviewRejectsNonTerminalEvidence();
    void doesNotUsePreparedFinalWhenReviewStalls();
    void stagnationRecoveryIsIndependentFromActionRepair();
    void compactsLongAgentContextAndPreservesEvidence();
    void hasNoToolCallCountLimit();
    void longRunWarningDoesNotStopAgent();
};

agent::ToolDefinition echoTool()
{
    return {QStringLiteral("fake.echo"),
            QStringLiteral("fake"),
            QStringLiteral("echo"),
            QStringLiteral("Echo values"),
            {{QStringLiteral("type"), QStringLiteral("object")}}};
}

agent::ToolDefinition namedTool(const QString& name, const QString& description)
{
    return {QStringLiteral("fake.") + name,
            QStringLiteral("fake"),
            name,
            description,
            {{QStringLiteral("type"), QStringLiteral("object")},
             {QStringLiteral("properties"),
              QJsonObject{{QStringLiteral("value"),
                           QJsonObject{{QStringLiteral("type"),
                                        QStringLiteral("string")}}}}}}};
}

QJsonObject planStep(const QString& id, const QString& description,
                     bool requiresTool)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("description"), description},
            {QStringLiteral("requires_tool"), requiresTool}};
}

QJsonObject reviewedStep(const QString& id, const QString& description,
                         bool requiresTool, const QString& status,
                         const QJsonArray& evidence = {})
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("description"), description},
            {QStringLiteral("requires_tool"), requiresTool},
            {QStringLiteral("status"), status},
            {QStringLiteral("evidence"), evidence}};
}

QByteArray taskPlanAction(const QJsonArray& steps)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("action"),
                                      QStringLiteral("task_plan")},
                                     {QStringLiteral("steps"), steps}})
        .toJson(QJsonDocument::Compact);
}

QByteArray completionReviewAction(const QString& verdict,
                                  const QJsonArray& steps,
                                  const QString& detail)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("action"),
                                      QStringLiteral("review_completion")},
                                     {QStringLiteral("verdict"), verdict},
                                     {QStringLiteral("steps"), steps},
                                     {QStringLiteral("detail"), detail}})
        .toJson(QJsonDocument::Compact);
}

void AgentControllerTest::toolCatalogIsStableAndValid()
{
    auto alpha =
        namedTool(QStringLiteral("alpha"), QStringLiteral("First tool"));
    alpha.outputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
    alpha.annotations = {{QStringLiteral("readOnlyHint"), true}};
    alpha.hasOutputSchema = true;
    const auto middle =
        namedTool(QStringLiteral("middle"), QStringLiteral("Middle tool"));
    const auto zeta =
        namedTool(QStringLiteral("zeta"), QStringLiteral("Last tool"));
    const auto first =
        application::ToolCatalogBuilder::build({zeta, alpha, middle}, 4'096);
    const auto second =
        application::ToolCatalogBuilder::build({middle, zeta, alpha}, 4'096);

    QCOMPARE(first.json, second.json);
    QCOMPARE(first.includedToolCount, 3);
    QCOMPARE(first.omittedToolCount, 0);
    QVERIFY(first.json.size() <= 4'096);

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(first.json, &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QVERIFY(document.isArray());
    const auto definitions = document.array();
    QCOMPARE(definitions.size(), 3);
    QCOMPARE(definitions.at(0).toObject().value(QStringLiteral("name")),
             QJsonValue(QStringLiteral("fake.alpha")));
    QCOMPARE(definitions.at(1).toObject().value(QStringLiteral("name")),
             QJsonValue(QStringLiteral("fake.middle")));
    QCOMPARE(definitions.at(2).toObject().value(QStringLiteral("name")),
             QJsonValue(QStringLiteral("fake.zeta")));
    QCOMPARE(definitions.at(0)
                 .toObject()
                 .value(QStringLiteral("inputSchema"))
                 .toObject(),
             alpha.inputSchema);
    QCOMPARE(definitions.at(0)
                 .toObject()
                 .value(QStringLiteral("outputSchema"))
                 .toObject(),
             alpha.outputSchema);
    QCOMPARE(definitions.at(0)
                 .toObject()
                 .value(QStringLiteral("annotations"))
                 .toObject(),
             alpha.annotations);
}

void AgentControllerTest::toolCatalogOmitsWholeDefinitions()
{
    const auto alpha =
        namedTool(QStringLiteral("alpha"), QStringLiteral("Small A"));
    const auto oversized =
        namedTool(QStringLiteral("middle"), QString(8'192, QLatin1Char('x')));
    const auto zeta =
        namedTool(QStringLiteral("zeta"), QStringLiteral("Small Z"));
    const auto expected =
        application::ToolCatalogBuilder::build({alpha, zeta}, 4'096);
    const auto actual = application::ToolCatalogBuilder::build(
        {zeta, oversized, alpha}, expected.json.size());

    QCOMPARE(actual.json, expected.json);
    QCOMPARE(actual.includedToolCount, 2);
    QCOMPARE(actual.omittedToolCount, 1);
    QVERIFY(actual.json.size() <= expected.json.size());
    QVERIFY(QJsonDocument::fromJson(actual.json).isArray());

    const auto empty = application::ToolCatalogBuilder::build({oversized}, 32);
    QCOMPARE(empty.json, QByteArrayLiteral("[]"));
    QCOMPARE(empty.includedToolCount, 0);
    QCOMPARE(empty.omittedToolCount, 1);
}

void AgentControllerTest::completesMultiStepToolRun()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    auto toolArgumentsValid = false;
    QList<chat::Message> lastMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int maxTokens)
            {
                ++generationCount;
                lastMessages = messages;
                QCOMPARE(maxTokens, 4'096);
            },
            [] {},
            [&](const QString& name, const QJsonObject& arguments)
            {
                ++toolCallCount;
                toolArgumentsValid =
                    name == QStringLiteral("fake.echo") &&
                    arguments.value(QStringLiteral("value")).toString() ==
                        QStringLiteral("hello");
                return QStringLiteral("tool-request-1");
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    const auto request =
        QStringLiteral("1. Use a tool.\n2. Report the result.");
    QVERIFY(controller.start(request, {}, {echoTool()}));
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 1);
    QVERIFY(lastMessages.constFirst().content.contains(
        QStringLiteral("fake.echo")));
    QVERIFY(
        lastMessages.constLast().content.contains(QStringLiteral("task_plan")));

    const QJsonArray plan{
        planStep(QStringLiteral("step-1"), QStringLiteral("Use a tool"), true),
        planStep(QStringLiteral("step-2"), QStringLiteral("Report the result"),
                 false)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 2);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":"hello"}})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 1);
    QVERIFY(toolArgumentsValid);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    result.result = {
        {QStringLiteral("content"), QStringLiteral("duplicate-marker")},
        {QStringLiteral("structuredContent"),
         QJsonObject{{QStringLiteral("value"), QStringLiteral("hello")}}}};
    controller.receiveToolResult(result);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 3);
    QVERIFY(lastMessages.constLast().content.contains(
        QStringLiteral("tool_result")));
    QVERIFY(lastMessages.constLast().content.contains(
        QStringLiteral("Do not repeat this exact call")));
    QVERIFY(lastMessages.constLast().content.contains(
        QStringLiteral("only this operation is complete")));
    QVERIFY(lastMessages.constLast().content.contains(QStringLiteral("hello")));
    QVERIFY(!lastMessages.constLast().content.contains(
        QStringLiteral("duplicate-marker")));

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Task complete"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 4);
    QCOMPARE(finalSpy.count(), 0);
    QVERIFY(lastMessages.constLast().content.contains(
        QStringLiteral("Completion review required")));
    QVERIFY(lastMessages.constLast().content.contains(request));

    const QJsonArray completedSteps{
        reviewedStep(QStringLiteral("step-1"), QStringLiteral("Use a tool"),
                     true, QStringLiteral("satisfied"), QJsonArray{1}),
        reviewedStep(QStringLiteral("step-2"),
                     QStringLiteral("Report the result"), false,
                     QStringLiteral("satisfied"), QJsonArray{1})};
    controller.receiveToken(completionReviewAction(
        QStringLiteral("complete"), completedSteps,
        QStringLiteral("All requested work is complete.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(finalSpy.count(), 1);
    QCOMPARE(finalSpy.at(0).at(1).toString(), QStringLiteral("Task complete"));
    QCOMPARE(finishedSpy.count(), 1);
    QVERIFY(!controller.hasActiveRun());
    QCOMPARE(controller.conversationMessages().size(), 2);
}

void AgentControllerTest::simpleToolRunCompletesWithoutReview()
{
    auto generationCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("tool-request-1"); }, [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(QStringLiteral("Read the current file."), {},
                             {echoTool()}));
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})"));
    controller.completeGeneration(false);
    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    controller.receiveToolResult(result);
    QCOMPARE(generationCount, 2);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Read complete"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(generationCount, 2);
    QCOMPARE(finalSpy.count(), 1);
}

void AgentControllerTest::simpleTaskRecoversFromFailureWithoutStructuredReview()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(QStringLiteral("Set the inlet flow rate."), {},
                             {echoTool()}));
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})"));
    controller.completeGeneration(false);
    agent::ToolResult failedResult;
    failedResult.requestId = QStringLiteral("tool-request-1");
    failedResult.serverId = QStringLiteral("fake");
    failedResult.toolName = QStringLiteral("echo");
    failedResult.isError = true;
    failedResult.errorMessage = QStringLiteral("Invalid value.");
    controller.receiveToolResult(failedResult);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"The flow rate is set."})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("latest operation is unfinished")));

    const auto completeSuccessfulCall = [&](int value, const QString& requestId)
    {
        controller.receiveToken(
            QStringLiteral(
                R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":%1}})")
                .arg(value)
                .toUtf8());
        controller.completeGeneration(false);
        agent::ToolResult result;
        result.requestId = requestId;
        result.serverId = QStringLiteral("fake");
        result.toolName = QStringLiteral("echo");
        controller.receiveToolResult(result);
    };
    completeSuccessfulCall(2, QStringLiteral("tool-request-2"));
    completeSuccessfulCall(3, QStringLiteral("tool-request-3"));
    QCOMPARE(controller.activeRun()->successfulToolResults, 2);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"The flow rate is set and verified."})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(finalSpy.count(), 1);
    QCOMPARE(generationCount, 5);
}

void AgentControllerTest::waitsForApprovalAndHonorsRejection()
{
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("unexpected");
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::RequireApproval; }});
    QSignalSpy approvalSpy(&controller,
                           &application::AgentController::approvalRequested);
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    QVERIFY(controller.start(QStringLiteral("Use a tool"), {}, {echoTool()}));
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{}})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(),
             application::AgentRun::State::WaitingForApproval);
    QCOMPARE(approvalSpy.count(), 1);
    QCOMPARE(toolCallCount, 0);

    controller.resolveApproval(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Failed);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(finishedSpy.at(0).at(2).toString(),
             QStringLiteral("approval_denied"));
}

void AgentControllerTest::repairsOnlyOneInvalidAction()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool");
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    QVERIFY(controller.start(QStringLiteral("Do work"), {}, {echoTool()}));
    controller.receiveToken(QByteArrayLiteral("not-json"));
    controller.completeGeneration(false);
    QCOMPARE(generationCount, 2);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);

    controller.receiveToken(QByteArrayLiteral("still-not-json"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Failed);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(finishedSpy.at(0).at(2).toString(),
             QStringLiteral("invalid_agent_action"));
}

void AgentControllerTest::cancelsAndIgnoresLateResponses()
{
    auto cancellationCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [&] { ++cancellationCount; }, [](const QString&, const QJsonObject&)
            { return QString{}; }, [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(QStringLiteral("Do work"), {}, {echoTool()}));
    controller.cancel();
    QCOMPARE(controller.state(), application::AgentRun::State::Cancelled);
    QCOMPARE(cancellationCount, 1);
    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"late"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Cancelled);
    QCOMPARE(finalSpy.count(), 0);
}

void AgentControllerTest::keepsConversationHistoryAndClearsIt()
{
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy clearedSpy(&controller,
                          &application::AgentController::conversationCleared);

    QVERIFY(controller.start(QStringLiteral("First question"), {}, {}));
    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"First answer"})"));
    controller.completeGeneration(false);
    QVERIFY(controller.hasConversation());

    QVERIFY(controller.start(QStringLiteral("Follow-up"), {}, {}));
    QCOMPARE(generatedMessages.size(), 4);
    QCOMPARE(generatedMessages.at(1).content, QStringLiteral("First question"));
    QCOMPARE(generatedMessages.at(2).content, QStringLiteral("First answer"));
    QCOMPARE(generatedMessages.at(3).content, QStringLiteral("Follow-up"));
    QVERIFY(!controller.setConversationMessages({}));
    controller.cancel();

    const QList<chat::Message> sharedHistory{
        {chat::Role::User, QStringLiteral("Shared question")},
        {chat::Role::Assistant, QStringLiteral("Shared answer")}};
    QVERIFY(controller.setConversationMessages(sharedHistory));
    QCOMPARE(controller.conversationMessages(), sharedHistory);

    QVERIFY(controller.clearConversation());
    QVERIFY(!controller.hasConversation());
    QCOMPARE(clearedSpy.count(), 1);
}

void AgentControllerTest::emptyToolPromptForbidsToolCalls()
{
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    QVERIFY(controller.start(QStringLiteral("Write a C++ file"), {}, {}));
    QVERIFY(!generatedMessages.isEmpty());
    const auto prompt = generatedMessages.constFirst().content;
    QVERIFY(prompt.contains(QStringLiteral("No tools are available")));
    QVERIFY(prompt.contains(QStringLiteral("must not call a tool")));
    QVERIFY(!prompt.contains(QStringLiteral("server.tool")));
    controller.cancel();
}

void AgentControllerTest::casualPromptPrefersFinalWithoutTools()
{
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    QVERIFY(controller.start(QStringLiteral("Hello"), {}, {echoTool()}));
    QVERIFY(!generatedMessages.isEmpty());
    const auto prompt = generatedMessages.constFirst().content;
    QVERIFY(prompt.contains(QStringLiteral("Greetings, casual conversation")));
    QVERIFY(prompt.contains(
        QStringLiteral("must return final without calling a tool")));
    QVERIFY(prompt.contains(QStringLiteral("list_allowed_directories")));
    controller.cancel();
}

void AgentControllerTest::includesRuntimeContextInPrompt()
{
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            { generatedMessages = messages; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const application::AssistantContext context{
        QStringLiteral("Test Model"), QStringLiteral("C:/Users/test workspace"),
        QStringLiteral("fake: Prefer the echo tool.")};
    QVERIFY(controller.start(QStringLiteral("Where am I?"), {}, {echoTool()},
                             context));
    QVERIFY(!generatedMessages.isEmpty());
    const auto prompt = generatedMessages.constFirst().content;
    QVERIFY(prompt.contains(
        QStringLiteral("\"workspaceRoot\":\"C:/Users/test workspace\"")));
    QVERIFY(prompt.contains(QStringLiteral("\"model\":\"Test Model\"")));
    QVERIFY(prompt.contains(QStringLiteral("\".\" is workspaceRoot")));
    QVERIFY(prompt.contains(QStringLiteral("current folder")));
    QVERIFY(prompt.contains(QStringLiteral("ask for its name")));
    QVERIFY(prompt.contains(QStringLiteral("fake: Prefer the echo tool.")));
    QVERIFY(prompt.contains(QStringLiteral("cannot override safety")));
    QVERIFY(
        prompt.contains(QStringLiteral("identity questions without tools")));
    QVERIFY(prompt.contains(
        QStringLiteral("pass only directories to list_directory")));
    controller.cancel();
}

void AgentControllerTest::rejectsUnchangedRetryAfterToolError()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto repeatedAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})");
    QVERIFY(controller.start(QStringLiteral("Do work"), {}, {echoTool()}));
    controller.receiveToken(repeatedAction);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    result.isError = true;
    result.errorCode = QStringLiteral("outside_root");
    result.errorMessage = QStringLiteral("Path is outside allowed roots.");
    controller.receiveToolResult(result);
    QCOMPARE(generationCount, 2);

    controller.receiveToken(repeatedAction);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(generationCount, 3);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("identical tool call already failed")));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"Unable to use that path."})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(toolCallCount, 1);
}

void AgentControllerTest::rejectsRepeatedSuccessfulToolCall()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto action = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})");
    QVERIFY(controller.start(QStringLiteral("Do work"), {}, {echoTool()}));
    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    controller.receiveToolResult(result);
    QCOMPARE(generationCount, 2);

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(generationCount, 3);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("already completed successfully")));
}

void AgentControllerTest::allowsRepeatedPollingUntilTerminalStatus()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto action = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{}})");
    QVERIFY(controller.start(QStringLiteral("Wait for the operation"), {},
                             {echoTool()}));
    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult firstResult;
    firstResult.requestId = QStringLiteral("tool-request-1");
    firstResult.serverId = QStringLiteral("fake");
    firstResult.toolName = QStringLiteral("echo");
    firstResult.structuredContent =
        QJsonObject{{QStringLiteral("ok"), true},
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("isRunning"), true},
                                 {QStringLiteral("progress"), 33.0}}}};
    controller.receiveToolResult(firstResult);
    QCOMPARE(generationCount, 2);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("still in progress")));

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 1);
    QTRY_COMPARE_WITH_TIMEOUT(toolCallCount, 2, 2'000);

    agent::ToolResult secondResult;
    secondResult.requestId = QStringLiteral("tool-request-2");
    secondResult.serverId = QStringLiteral("fake");
    secondResult.toolName = QStringLiteral("echo");
    secondResult.result = {
        {QStringLiteral("structuredContent"),
         QJsonObject{{QStringLiteral("status"), QStringLiteral("in_progress")},
                     {QStringLiteral("progress"), 66.0}}}};
    controller.receiveToolResult(secondResult);

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(toolCallCount, 2);
    QTRY_COMPARE_WITH_TIMEOUT(toolCallCount, 3, 2'000);

    agent::ToolResult terminalResult;
    terminalResult.requestId = QStringLiteral("tool-request-3");
    terminalResult.serverId = QStringLiteral("fake");
    terminalResult.toolName = QStringLiteral("echo");
    terminalResult.structuredContent =
        QJsonObject{{QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("isRunning"), false}}}};
    controller.receiveToolResult(terminalResult);

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 3);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("already completed successfully")));
    controller.cancel();
}

void AgentControllerTest::cancelsPendingStatusPoll()
{
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto action = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{}})");
    QVERIFY(controller.start(QStringLiteral("Wait for the operation"), {},
                             {echoTool()}));
    controller.receiveToken(action);
    controller.completeGeneration(false);

    agent::ToolResult runningResult;
    runningResult.requestId = QStringLiteral("tool-request-1");
    runningResult.serverId = QStringLiteral("fake");
    runningResult.toolName = QStringLiteral("echo");
    runningResult.structuredContent =
        QJsonObject{{QStringLiteral("isRunning"), true}};
    controller.receiveToolResult(runningResult);

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 1);
    controller.cancel();
    QCOMPARE(controller.state(), application::AgentRun::State::Cancelled);
    QTest::qWait(1'100);
    QCOMPARE(toolCallCount, 1);
}

void AgentControllerTest::rejectsAlternatingCompletedToolCycle()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto openAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"operation":"open"}})");
    const auto closeAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"operation":"close"}})");
    QVERIFY(controller.start(QStringLiteral("Keep working in the case"), {},
                             {echoTool()}));

    const auto completeTool =
        [&](const QByteArray& action, const QString& requestId, bool isError)
    {
        controller.receiveToken(action);
        controller.completeGeneration(false);
        QCOMPARE(controller.state(),
                 application::AgentRun::State::ExecutingTool);
        agent::ToolResult result;
        result.requestId = requestId;
        result.serverId = QStringLiteral("fake");
        result.toolName = QStringLiteral("echo");
        result.isError = isError;
        if (isError)
        {
            result.errorCode = QStringLiteral("operation_failed");
            result.errorMessage = QStringLiteral("Operation failed.");
        }
        controller.receiveToolResult(result);
        QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    };

    completeTool(openAction, QStringLiteral("tool-request-1"), false);
    completeTool(closeAction, QStringLiteral("tool-request-2"), true);
    completeTool(openAction, QStringLiteral("tool-request-3"), false);
    QCOMPARE(toolCallCount, 3);

    controller.receiveToken(closeAction);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 3);
    QCOMPARE(generationCount, 5);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("repeated cycle of completed calls")));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("toggle resources open and closed")));
}

void AgentControllerTest::reviewsPrematureFinalBeforeAnyToolCall()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-1");
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(
        QStringLiteral("1. Create the case.\n2. Run the simulation."), {},
        {echoTool()}));
    const QJsonArray plan{planStep(QStringLiteral("create"),
                                   QStringLiteral("Create the case"), true),
                          planStep(QStringLiteral("simulate"),
                                   QStringLiteral("Run the simulation"), true)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"I still need to inspect the files and create the case."})"));
    controller.completeGeneration(false);

    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 3);
    QCOMPARE(toolCallCount, 0);
    QCOMPARE(finalSpy.count(), 0);
    QCOMPARE(controller.activeRun()->successfulToolResults, 0);
    QVERIFY(controller.activeRun()->awaitingCompletionReview);
    QVERIFY(!controller.activeRun()->pendingFinalCandidate.isEmpty());
    QCOMPARE(controller.activeRun()->completionSteps, plan);
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("Do not return another final action")));
    QVERIFY(generatedMessages.constLast().content.contains(
        QStringLiteral("successful terminal evidence")));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{}})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::ExecutingTool);
    QCOMPARE(toolCallCount, 1);

    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    controller.receiveToolResult(result);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"The requested work is complete."})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);

    const QJsonArray completedSteps{
        reviewedStep(QStringLiteral("create"),
                     QStringLiteral("Create the case"), true,
                     QStringLiteral("satisfied"), QJsonArray{1}),
        reviewedStep(QStringLiteral("simulate"),
                     QStringLiteral("Run the simulation"), true,
                     QStringLiteral("satisfied"), QJsonArray{1})};
    controller.receiveToken(
        completionReviewAction(QStringLiteral("complete"), completedSteps,
                               QStringLiteral("All work is complete.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(finalSpy.count(), 1);
}

void AgentControllerTest::reviewsCompletionAfterEachEvidenceRevision()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(QStringLiteral("1. Do the first operation.\n"
                                            "2. Do the second operation."),
                             {}, {echoTool()}));
    const QJsonArray plan{
        planStep(QStringLiteral("first"),
                 QStringLiteral("Do the first operation"), true),
        planStep(QStringLiteral("second"),
                 QStringLiteral("Do the second operation"), true)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})"));
    controller.completeGeneration(false);
    agent::ToolResult firstResult;
    firstResult.requestId = QStringLiteral("tool-request-1");
    firstResult.serverId = QStringLiteral("fake");
    firstResult.toolName = QStringLiteral("echo");
    controller.receiveToolResult(firstResult);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 4);
    QCOMPARE(controller.activeRun()->lastReviewedEvidenceRevision, 1);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":2}})"));
    controller.completeGeneration(false);
    agent::ToolResult secondResult;
    secondResult.requestId = QStringLiteral("tool-request-2");
    secondResult.serverId = QStringLiteral("fake");
    secondResult.toolName = QStringLiteral("echo");
    controller.receiveToolResult(secondResult);
    QCOMPARE(generationCount, 5);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 6);
    QCOMPARE(controller.activeRun()->lastReviewedEvidenceRevision, 2);

    const QJsonArray completedSteps{
        reviewedStep(QStringLiteral("first"),
                     QStringLiteral("Do the first operation"), true,
                     QStringLiteral("satisfied"), QJsonArray{1}),
        reviewedStep(QStringLiteral("second"),
                     QStringLiteral("Do the second operation"), true,
                     QStringLiteral("satisfied"), QJsonArray{2})};
    controller.receiveToken(completionReviewAction(
        QStringLiteral("complete"), completedSteps,
        QStringLiteral("Both operations are complete.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(generationCount, 6);
    QCOMPARE(finalSpy.count(), 1);
}

void AgentControllerTest::rejectsRepeatedFinalDuringCompletionReview()
{
    auto generationCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    QVERIFY(controller.start(
        QStringLiteral("1. Create the case.\n2. Run the simulation."), {},
        {echoTool()}));
    const QJsonArray plan{planStep(QStringLiteral("create"),
                                   QStringLiteral("Create the case"), true),
                          planStep(QStringLiteral("simulate"),
                                   QStringLiteral("Run the simulation"), true)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);

    const auto final = QByteArrayLiteral(
        R"({"action":"final","content":"Everything is complete."})");
    controller.receiveToken(final);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 3);

    controller.receiveToken(final);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 4);
    QCOMPARE(finalSpy.count(), 0);
    QVERIFY(controller.activeRun()->awaitingCompletionReview);

    controller.receiveToken(final);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Failed);
    QCOMPARE(finalSpy.count(), 0);
    QCOMPARE(finishedSpy.constFirst().at(2).toString(),
             QStringLiteral("completion_unverified"));
}

void AgentControllerTest::completionReviewRejectsNonTerminalEvidence()
{
    auto generationCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("tool-request-1"); }, [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(
        QStringLiteral("1. Wait for completion.\n2. Report the result."), {},
        {echoTool()}));
    const QJsonArray plan{planStep(QStringLiteral("wait"),
                                   QStringLiteral("Wait for completion"), true),
                          planStep(QStringLiteral("report"),
                                   QStringLiteral("Report the result"), false)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{}})"));
    controller.completeGeneration(false);

    agent::ToolResult runningResult;
    runningResult.requestId = QStringLiteral("tool-request-1");
    runningResult.serverId = QStringLiteral("fake");
    runningResult.toolName = QStringLiteral("echo");
    runningResult.structuredContent =
        QJsonObject{{QStringLiteral("isRunning"), true},
                    {QStringLiteral("progress"), 50.0}};
    controller.receiveToolResult(runningResult);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"The operation completed."})"));
    controller.completeGeneration(false);
    const QJsonArray invalidCompletedSteps{
        reviewedStep(QStringLiteral("wait"),
                     QStringLiteral("Wait for completion"), true,
                     QStringLiteral("satisfied"), QJsonArray{1}),
        reviewedStep(QStringLiteral("report"),
                     QStringLiteral("Report the result"), false,
                     QStringLiteral("satisfied"), QJsonArray{1})};
    controller.receiveToken(completionReviewAction(
        QStringLiteral("complete"), invalidCompletedSteps,
        QStringLiteral("The operation completed.")));
    controller.completeGeneration(false);

    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);
    QCOMPARE(controller.activeRun()->completionReviewFailures, 1);
    QCOMPARE(generationCount, 5);

    const QJsonArray pendingSteps{
        reviewedStep(QStringLiteral("wait"),
                     QStringLiteral("Wait for completion"), true,
                     QStringLiteral("pending"), QJsonArray{1}),
        reviewedStep(QStringLiteral("report"),
                     QStringLiteral("Report the result"), false,
                     QStringLiteral("pending"), QJsonArray{})};
    controller.receiveToken(completionReviewAction(
        QStringLiteral("continue"), pendingSteps,
        QStringLiteral(
            "Poll the operation until it reaches a terminal state.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);
    controller.cancel();
}

void AgentControllerTest::doesNotUsePreparedFinalWhenReviewStalls()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    const auto repeatedAction = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"path":"controlData.json"}})");
    QVERIFY(controller.start(
        QStringLiteral("1. Read controlData.json.\n2. Report its state."), {},
        {echoTool()}));
    const QJsonArray plan{
        planStep(QStringLiteral("read"),
                 QStringLiteral("Read controlData.json"), true),
        planStep(QStringLiteral("report"), QStringLiteral("Report its state"),
                 false)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(repeatedAction);
    controller.completeGeneration(false);
    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    controller.receiveToolResult(result);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Prepared answer"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 4);

    controller.receiveToken(repeatedAction);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(generationCount, 5);
    QCOMPARE(finalSpy.count(), 0);

    controller.receiveToken(repeatedAction);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Failed);
    QCOMPARE(toolCallCount, 1);
    QCOMPARE(finalSpy.count(), 0);
}

void AgentControllerTest::stagnationRecoveryIsIndependentFromActionRepair()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>&, const models::InferencePreset&,
                int) { ++generationCount; },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    const auto action = QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})");
    QVERIFY(
        controller.start(QStringLiteral("Read one value."), {}, {echoTool()}));
    controller.receiveToken(QByteArrayLiteral("invalid-json"));
    controller.completeGeneration(false);
    QCOMPARE(generationCount, 2);
    QCOMPARE(controller.activeRun()->repairAttempts, 1);

    controller.receiveToken(action);
    controller.completeGeneration(false);
    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    controller.receiveToolResult(result);
    QCOMPARE(generationCount, 3);

    controller.receiveToken(action);
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(controller.activeRun()->repairAttempts, 1);
    QCOMPARE(controller.activeRun()->stagnationRecoveries, 1);
    QCOMPARE(generationCount, 4);
    QCOMPARE(toolCallCount, 1);

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Done"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
}

void AgentControllerTest::compactsLongAgentContextAndPreservesEvidence()
{
    auto generationCount = 0;
    auto toolCallCount = 0;
    QList<chat::Message> generatedMessages;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [&](const QList<chat::Message>& messages,
                const models::InferencePreset&, int)
            {
                ++generationCount;
                generatedMessages = messages;
            },
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});

    models::InferencePreset preset;
    preset.contextSize = 4'096;
    preset.maxOutputTokens = 512;
    const auto request = QStringLiteral(
        "1. Create case1 and preserve every real path and UUID.\n"
        "2. Create the shaft region.");
    QVERIFY(controller.start(request, preset, {echoTool()}));
    const QJsonArray plan{
        planStep(QStringLiteral("case"),
                 QStringLiteral("Create case1 and preserve its path and UUID"),
                 true),
        planStep(QStringLiteral("shaft"),
                 QStringLiteral("Create the shaft region"), true)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"case_path":"E:/stl_case1/case1"}})"));
    controller.completeGeneration(false,
                                  {{QStringLiteral("promptTokens"), 1'900}});

    const auto uuid = QStringLiteral("713b9921-7b45-45a5-aad3-0b8bc89fced6");
    const auto path = QStringLiteral("E:/stl_case1/case1");
    agent::ToolResult result;
    result.requestId = QStringLiteral("tool-request-1");
    result.serverId = QStringLiteral("fake");
    result.toolName = QStringLiteral("echo");
    result.result = {
        {QStringLiteral("structuredContent"),
         QJsonObject{{QStringLiteral("ok"), true},
                     {QStringLiteral("uuid"), uuid},
                     {QStringLiteral("case_path"), path},
                     {QStringLiteral("large_payload"),
                      QStringLiteral("mesh-data-").repeated(2'000)}}}};
    controller.receiveToolResult(result);

    QCOMPARE(generationCount, 3);
    QVERIFY(controller.activeRun().has_value());
    QCOMPARE(controller.activeRun()->contextCompactions, 1);
    QCOMPARE(generatedMessages.constFirst().role, chat::Role::System);
    QCOMPARE(generatedMessages.constLast().role, chat::Role::User);
    const auto compactedTask = generatedMessages.constLast().content;
    QVERIFY(compactedTask.contains(request));
    QVERIFY(compactedTask.contains(QStringLiteral("<agent_progress>")));
    QVERIFY(compactedTask.contains(QStringLiteral("qtllm-agent-progress-v2")));
    QVERIFY(compactedTask.contains(QStringLiteral("Create the shaft region")));
    QVERIFY(compactedTask.contains(uuid));
    QVERIFY(compactedTask.contains(path));
    QVERIFY(compactedTask.size() < 5'000);
    QVERIFY(
        !compactedTask.contains(QStringLiteral("mesh-data-").repeated(100)));

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"call_tool","tool":"fake.echo","arguments":{"region_name":"shaft"}})"));
    controller.completeGeneration(false,
                                  {{QStringLiteral("promptTokens"), 2'200}});
    const auto secondUuid =
        QStringLiteral("af0af18a-6758-429f-b6cb-e161cc9d64d2");
    agent::ToolResult secondResult;
    secondResult.requestId = QStringLiteral("tool-request-2");
    secondResult.serverId = QStringLiteral("fake");
    secondResult.toolName = QStringLiteral("echo");
    secondResult.result = {
        {QStringLiteral("structuredContent"),
         QJsonObject{{QStringLiteral("ok"), true},
                     {QStringLiteral("uuid"), secondUuid},
                     {QStringLiteral("region_name"), QStringLiteral("shaft")},
                     {QStringLiteral("large_payload"),
                      QStringLiteral("region-data-").repeated(2'000)}}}};
    controller.receiveToolResult(secondResult);

    QCOMPARE(generationCount, 4);
    QCOMPARE(controller.activeRun()->contextCompactions, 2);
    const auto twiceCompactedTask = generatedMessages.constLast().content;
    QCOMPARE(twiceCompactedTask.count(QStringLiteral("<agent_progress>")), 1);
    QCOMPARE(twiceCompactedTask.count(request), 1);
    QVERIFY(twiceCompactedTask.contains(uuid));
    QVERIFY(twiceCompactedTask.contains(secondUuid));
    controller.cancel();
}

void AgentControllerTest::hasNoToolCallCountLimit()
{
    auto toolCallCount = 0;
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {},
            [&](const QString&, const QJsonObject&)
            {
                ++toolCallCount;
                return QStringLiteral("tool-request-%1").arg(toolCallCount);
            },
            [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);
    QSignalSpy finalSpy(&controller,
                        &application::AgentController::finalAnswerReady);

    QVERIFY(controller.start(
        QStringLiteral("1. Perform all operations.\n2. Report completion."), {},
        {echoTool()}));
    const QJsonArray plan{
        planStep(QStringLiteral("operate"),
                 QStringLiteral("Perform all operations"), true),
        planStep(QStringLiteral("report"), QStringLiteral("Report completion"),
                 false)};
    controller.receiveToken(taskPlanAction(plan));
    controller.completeGeneration(false);
    for (auto index = 0; index < 20; ++index)
    {
        controller.receiveToken(
            QStringLiteral(
                R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":%1}})")
                .arg(index)
                .toUtf8());
        controller.completeGeneration(false);
        QCOMPARE(controller.state(),
                 application::AgentRun::State::ExecutingTool);

        agent::ToolResult result;
        result.requestId = QStringLiteral("tool-request-%1").arg(index + 1);
        result.serverId = QStringLiteral("fake");
        result.toolName = QStringLiteral("echo");
        result.result = {{QStringLiteral("content"), index}};
        controller.receiveToolResult(result);
        QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    }
    QCOMPARE(toolCallCount, 20);

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"All operations completed"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(finalSpy.count(), 0);

    QJsonArray allEvidence;
    for (auto sequence = 1; sequence <= 20; ++sequence)
        allEvidence.append(sequence);
    const QJsonArray completedSteps{
        reviewedStep(QStringLiteral("operate"),
                     QStringLiteral("Perform all operations"), true,
                     QStringLiteral("satisfied"), allEvidence),
        reviewedStep(QStringLiteral("report"),
                     QStringLiteral("Report completion"), false,
                     QStringLiteral("satisfied"), QJsonArray{20})};
    controller.receiveToken(
        completionReviewAction(QStringLiteral("complete"), completedSteps,
                               QStringLiteral("All operations completed.")));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(toolCallCount, 20);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.constFirst().at(2).toString(), QString{});
    QCOMPARE(finalSpy.count(), 1);
}

void AgentControllerTest::longRunWarningDoesNotStopAgent()
{
    application::AgentController controller(
        application::AgentController::Dependencies{
            [](const QList<chat::Message>&, const models::InferencePreset&,
               int) {},
            [] {}, [](const QString&, const QJsonObject&)
            { return QStringLiteral("unused"); }, [](const QString&) {},
            [](const QString&, const QJsonObject&, QString&) { return true; },
            [](const QString&)
            { return infrastructure::mcp::ToolDecision::Allow; }});
    QSignalSpy eventSpy(&controller,
                        &application::AgentController::eventRecorded);
    QSignalSpy finishedSpy(&controller,
                           &application::AgentController::runFinished);

    QVERIFY(controller.start(QStringLiteral("Long-running task"), {},
                             {echoTool()}));
    QVERIFY(QMetaObject::invokeMethod(&controller, "notifyLongRunning",
                                      Qt::DirectConnection));
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QVERIFY(controller.hasActiveRun());
    QCOMPARE(finishedSpy.count(), 0);
    const auto warning =
        qvariant_cast<agent::Event>(eventSpy.constLast().constFirst());
    QCOMPARE(warning.type, agent::EventType::Warning);
    QVERIFY(warning.message.contains(QStringLiteral("continue")));
    controller.cancel();
}
}  // namespace qtllm::tests

QTEST_GUILESS_MAIN(qtllm::tests::AgentControllerTest)

#include "AgentControllerTest.moc"
