#include "AgentController.hpp"

#include <QSignalSpy>
#include <QtTest>

namespace qtllm::tests
{
class AgentControllerTest final : public QObject
{
    Q_OBJECT

   private slots:
    void completesMultiStepToolRun();
    void waitsForApprovalAndHonorsRejection();
    void repairsOnlyOneInvalidAction();
    void cancelsAndIgnoresLateResponses();
    void keepsConversationHistoryAndClearsIt();
    void emptyToolPromptForbidsToolCalls();
    void casualPromptPrefersFinalWithoutTools();
    void includesRuntimeContextInPrompt();
    void rejectsUnchangedRetryAfterToolError();
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

    QVERIFY(controller.start(QStringLiteral("Use a tool"), {}, {echoTool()}));
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 1);
    QVERIFY(lastMessages.constFirst().content.contains(
        QStringLiteral("fake.echo")));

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
    QCOMPARE(generationCount, 2);
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
    QCOMPARE(generationCount, 3);
    QCOMPARE(finalSpy.count(), 0);
    QVERIFY(lastMessages.constLast().content.contains(
        QStringLiteral("Completion review required")));
    QVERIFY(lastMessages.constLast().content.contains(
        QStringLiteral("<original_request>Use a tool</original_request>")));

    controller.receiveToken(
        QByteArrayLiteral(R"({"action":"final","content":"Task complete"})"));
    controller.completeGeneration(false);
    QCOMPARE(controller.state(), application::AgentRun::State::Completed);
    QCOMPARE(finalSpy.count(), 1);
    QCOMPARE(finalSpy.at(0).at(1).toString(), QStringLiteral("Task complete"));
    QCOMPARE(finishedSpy.count(), 1);
    QVERIFY(!controller.hasActiveRun());
    QCOMPARE(controller.conversationMessages().size(), 2);
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

    QVERIFY(controller.start(QStringLiteral("Do several operations"), {},
                             {echoTool()}));
    for (auto index = 0; index < 20; ++index)
    {
        controller.receiveToken(QByteArrayLiteral(
            R"({"action":"call_tool","tool":"fake.echo","arguments":{"value":1}})"));
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

    controller.receiveToken(QByteArrayLiteral(
        R"({"action":"final","content":"All operations completed"})"));
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
