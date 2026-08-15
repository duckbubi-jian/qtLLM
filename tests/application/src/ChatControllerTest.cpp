#include "ChatController.hpp"

#include <QSignalSpy>
#include <QtTest>

namespace qtllm::tests
{
class ChatControllerTest final : public QObject
{
    Q_OBJECT

   private slots:
    void buildsRequestsAndStoresCompletedAnswers();
    void rollsBackCancelledAndFailedPrompts();
    void cancelsAndClearsOnlyAtValidTimes();
};

void ChatControllerTest::buildsRequestsAndStoresCompletedAnswers()
{
    QList<chat::Message> requestMessages;
    models::InferencePreset requestPreset;
    auto generateCount = 0;
    application::ChatController controller(
        [&](const QList<chat::Message>& messages,
            const models::InferencePreset& preset)
        {
            ++generateCount;
            requestMessages = messages;
            requestPreset = preset;
        },
        [] {});

    models::InferencePreset preset;
    preset.contextSize = 4096;
    preset.maxOutputTokens = 512;
    const application::AssistantContext context{QStringLiteral("Test Model"),
                                                QStringLiteral("C:/workspace")};
    QVERIFY(
        controller.sendPrompt(QStringLiteral("  Hello  "), preset, context));
    QCOMPARE(generateCount, 1);
    QCOMPARE(requestPreset.contextSize, 4096);
    QCOMPARE(requestPreset.maxOutputTokens, 512);
    QCOMPARE(requestMessages.size(), 2);
    QCOMPARE(requestMessages.at(0).role, chat::Role::System);
    QVERIFY(requestMessages.at(0).content.contains(
        QStringLiteral("\"model\":\"Test Model\"")));
    QVERIFY(requestMessages.at(0).content.contains(
        QStringLiteral("\"workspaceRoot\":\"C:/workspace\"")));
    QVERIFY(!requestMessages.at(0).content.contains(
        QStringLiteral("Available tools")));
    const chat::Message expectedUser{chat::Role::User, QStringLiteral("Hello")};
    QCOMPARE(requestMessages.at(1), expectedUser);

    controller.receiveToken(
        QByteArrayLiteral("<think>private reasoning</think>Visible answer"));
    controller.completeGeneration(false, {});
    QCOMPARE(controller.conversationMessages().size(), 2);
    const chat::Message expectedAssistant{chat::Role::Assistant,
                                          QStringLiteral("Visible answer")};
    QCOMPARE(controller.conversationMessages().at(1), expectedAssistant);

    const QList<chat::Message> sharedHistory{
        {chat::Role::User, QStringLiteral("Shared question")},
        {chat::Role::Assistant, QStringLiteral("Shared answer")}};
    QVERIFY(controller.setConversationMessages(sharedHistory));
    QCOMPARE(controller.conversationMessages(), sharedHistory);

    QVERIFY(controller.sendPrompt(QStringLiteral("Next"), preset));
    QCOMPARE(requestMessages.size(), 4);
    QCOMPARE(requestMessages.at(1), sharedHistory.at(0));
    QCOMPARE(requestMessages.at(2), sharedHistory.at(1));
    QCOMPARE(requestMessages.at(3).content, QStringLiteral("Next"));
}

void ChatControllerTest::rollsBackCancelledAndFailedPrompts()
{
    application::ChatController controller(
        [](const QList<chat::Message>&, const models::InferencePreset&) {},
        [] {});

    QVERIFY(controller.sendPrompt(QStringLiteral("Cancel me"), {}));
    controller.receiveToken(QByteArrayLiteral("partial"));
    controller.completeGeneration(true, {});
    QVERIFY(controller.conversationMessages().isEmpty());

    QSignalSpy errorSpy(&controller,
                        &application::ChatController::errorOccurred);
    QVERIFY(controller.sendPrompt(QStringLiteral("Retry me"), {}));
    controller.handleError(QStringLiteral("generation_failed"),
                           QStringLiteral("failed"));
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.constFirst().at(2).toString(),
             QStringLiteral("Retry me"));
    QVERIFY(controller.conversationMessages().isEmpty());
    QVERIFY(!controller.isGenerating());
}

void ChatControllerTest::cancelsAndClearsOnlyAtValidTimes()
{
    auto cancelCount = 0;
    application::ChatController controller(
        [](const QList<chat::Message>&, const models::InferencePreset&) {},
        [&] { ++cancelCount; });
    QSignalSpy clearSpy(&controller,
                        &application::ChatController::conversationCleared);

    controller.cancel();
    QCOMPARE(cancelCount, 0);
    QVERIFY(controller.sendPrompt(QStringLiteral("Hello"), {}));
    QVERIFY(!controller.clearConversation());
    controller.cancel();
    QCOMPARE(cancelCount, 1);
    controller.receiveToken(QByteArrayLiteral("Answer"));
    controller.completeGeneration(false, {});
    QVERIFY(controller.hasConversation());
    QVERIFY(controller.clearConversation());
    QCOMPARE(clearSpy.count(), 1);
    QVERIFY(!controller.hasConversation());
}
}  // namespace qtllm::tests

QTEST_GUILESS_MAIN(qtllm::tests::ChatControllerTest)

#include "ChatControllerTest.moc"
