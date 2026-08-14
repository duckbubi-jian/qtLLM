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
    result.result = {{QStringLiteral("content"), QStringLiteral("hello")}};
    controller.receiveToolResult(result);
    QCOMPARE(controller.state(), application::AgentRun::State::Deciding);
    QCOMPARE(generationCount, 2);
    QVERIFY(lastMessages.constLast().content.contains(
        QStringLiteral("tool_result")));

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
    controller.cancel();

    QVERIFY(controller.clearConversation());
    QVERIFY(!controller.hasConversation());
    QCOMPARE(clearedSpy.count(), 1);
}
}  // namespace qtllm::tests

QTEST_GUILESS_MAIN(qtllm::tests::AgentControllerTest)

#include "AgentControllerTest.moc"
