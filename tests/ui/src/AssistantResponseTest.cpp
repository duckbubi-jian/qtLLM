#include "AssistantResponse.hpp"
#include "AutoHideTabWidget.hpp"
#include "ChatView.hpp"
#include "MainWindow.hpp"
#include "MessageWidget.hpp"
#include "Theme.hpp"
#include "ToolApprovalWidget.hpp"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QEnterEvent>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextFormat>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtTest>

namespace qtllm::tests
{
class AssistantResponseTest final : public QObject
{
    Q_OBJECT

   private slots:
    void initTestCase();
    void parsesCompletedReasoning();
    void parsesStreamingReasoning();
    void preservesPlainAnswer();
    void excludesIncompleteReasoningFromHistory();
    void userMessageDisplaysItsText();
    void usesDistinctMessageLayoutsAndMarkdownStyle();
    void rendersConversationPreview();
    void reasoningOnlyResponseFallsBackToVisibleAnswer();
    void showsInlineToolApprovalAndRedactsSecrets();
    void showsAgentActivityUntilRunEnds();
    void recordsRedactedAgentActivity();
    void enterSendsAndShiftEnterAddsNewline();
    void restoresPromptAfterGenerationError();
    void clearsVisibleAndInMemoryConversation();
    void showsWorkspaceAsComposerLink();
    void separatesModelLocationAndReloadActions();
    void placesModelControlsInComposerAndMergesPrimaryAction();
};

void AssistantResponseTest::initTestCase()
{
    ui::applyApplicationTheme(*qApp);
}

void AssistantResponseTest::parsesCompletedReasoning()
{
    const auto response = chat::parseAssistantResponse(
        QStringLiteral("<think>internal notes</think>\nFinal **answer**"));
    QVERIFY(response.hasReasoning);
    QVERIFY(response.reasoningComplete);
    QCOMPARE(response.reasoning, QStringLiteral("internal notes"));
    QCOMPARE(response.answer, QStringLiteral("Final **answer**"));
}

void AssistantResponseTest::parsesStreamingReasoning()
{
    const auto response =
        chat::parseAssistantResponse(QStringLiteral("<think>still working"));
    QVERIFY(response.hasReasoning);
    QVERIFY(!response.reasoningComplete);
    QCOMPARE(response.reasoning, QStringLiteral("still working"));
    QVERIFY(response.answer.isEmpty());
}

void AssistantResponseTest::preservesPlainAnswer()
{
    const auto response = chat::parseAssistantResponse(
        QStringLiteral("```python\nprint('ok')\n```"));
    QVERIFY(!response.hasReasoning);
    QCOMPARE(response.answer, QStringLiteral("```python\nprint('ok')\n```"));
}

void AssistantResponseTest::excludesIncompleteReasoningFromHistory()
{
    QVERIFY(chat::assistantHistoryText(QStringLiteral("<think>still working"))
                .isEmpty());
    QCOMPARE(chat::assistantHistoryText(
                 QStringLiteral("<think>notes</think>Final answer")),
             QStringLiteral("Final answer"));
    QCOMPARE(chat::assistantHistoryText(QStringLiteral("Plain answer")),
             QStringLiteral("Plain answer"));
}

void AssistantResponseTest::userMessageDisplaysItsText()
{
    ui::MessageWidget message(ui::MessageWidget::Role::User);
    message.resize(800, 100);
    message.setUserText(QStringLiteral("visible question"));
    message.show();
    QTest::qWait(20);

    const auto browsers = message.findChildren<QTextBrowser*>();
    auto* body = static_cast<QTextBrowser*>(nullptr);
    for (auto* browser : browsers)
    {
        if (browser->toPlainText() == QStringLiteral("visible question"))
        {
            body = browser;
            break;
        }
    }
    QVERIFY(body != nullptr);
    QVERIFY(body->isVisible());
    QVERIFY(body->viewport()->height() >= body->fontMetrics().height());
}

void AssistantResponseTest::usesDistinctMessageLayoutsAndMarkdownStyle()
{
    ui::MessageWidget user(ui::MessageWidget::Role::User);
    user.resize(900, 120);
    user.setUserText(QStringLiteral("short question"));
    user.show();

    ui::MessageWidget assistant(ui::MessageWidget::Role::Assistant);
    assistant.resize(900, 180);
    assistant.setAssistantText(
        QStringLiteral("## Result\n\n> Note\n\n```cpp\nreturn 0;\n```"), true);
    assistant.show();
    QTest::qWait(20);

    auto* userBody =
        user.findChild<QTextBrowser*>(QStringLiteral("userMessageBody"));
    auto* assistantBody = assistant.findChild<QTextBrowser*>(
        QStringLiteral("assistantMessageBody"));
    QVERIFY(userBody != nullptr);
    QVERIFY(assistantBody != nullptr);
    QVERIFY(userBody->width() <= user.width() * 3 / 4);
    QVERIFY(userBody->geometry().center().x() > user.width() / 2);
    QVERIFY(assistantBody->width() > userBody->width());
    QVERIFY(assistantBody->document()->defaultStyleSheet().contains(
        QStringLiteral("blockquote")));
    QVERIFY(assistantBody->document()->defaultStyleSheet().contains(
        QStringLiteral("pre")));

    auto hasStyledCodeBlock = false;
    for (auto block = assistantBody->document()->begin(); block.isValid();
         block = block.next())
    {
        const auto format = block.blockFormat();
        if (!format.hasProperty(QTextFormat::BlockCodeFence) &&
            !format.hasProperty(QTextFormat::BlockCodeLanguage))
            continue;
        hasStyledCodeBlock =
            format.background().color() == QColor(QStringLiteral("#1f2937"));
        break;
    }
    QVERIFY(hasStyledCodeBlock);
}

void AssistantResponseTest::rendersConversationPreview()
{
    QWidget preview;
    preview.setObjectName(QStringLiteral("conversationContent"));
    preview.resize(900, 540);
    auto* layout = new QVBoxLayout(&preview);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(2);

    auto* user = new ui::MessageWidget(ui::MessageWidget::Role::User, &preview);
    user->setUserText(QStringLiteral("Please show a short C++ example."));
    layout->addWidget(user);

    auto* approval = new ui::ToolApprovalWidget(
        infrastructure::mcp::ToolRisk::ModifiesData,
        QStringLiteral("filesystem.write_file"),
        {{QStringLiteral("path"), QStringLiteral("D:/safe/example.cpp")},
         {QStringLiteral("content"),
          QStringLiteral("int add(int left, int right);")}},
        &preview);
    layout->addWidget(approval);

    auto* assistant =
        new ui::MessageWidget(ui::MessageWidget::Role::Assistant, &preview);
    assistant->setAssistantText(
        QStringLiteral("## Example\n\nHere is a compact function:\n\n"
                       "```cpp\nint add(int left, int right)\n{\n"
                       "    return left + right;\n}\n```\n\n"
                       "> The inputs are not modified."),
        true);
    layout->addWidget(assistant);
    layout->addStretch();

    preview.show();
    QTest::qWait(30);
    const auto image = preview.grab();
    QVERIFY(!image.isNull());

    const auto screenshotPath =
        qEnvironmentVariable("QTLLM_TEST_UI_SCREENSHOT");
    if (!screenshotPath.isEmpty())
    {
        QVERIFY(QDir().mkpath(QFileInfo(screenshotPath).absolutePath()));
        QVERIFY2(image.save(screenshotPath), qPrintable(screenshotPath));
    }
}

void AssistantResponseTest::reasoningOnlyResponseFallsBackToVisibleAnswer()
{
    ui::MessageWidget message(ui::MessageWidget::Role::Assistant);
    message.resize(800, 100);
    message.setAssistantText(QStringLiteral("<think>partial conclusion"), true);
    message.show();
    QTest::qWait(20);

    const auto browsers = message.findChildren<QTextBrowser*>();
    auto* visibleAnswer = static_cast<QTextBrowser*>(nullptr);
    for (auto* browser : browsers)
    {
        if (browser->isVisible() &&
            browser->toPlainText() == QStringLiteral("partial conclusion"))
        {
            visibleAnswer = browser;
            break;
        }
    }
    QVERIFY(visibleAnswer != nullptr);
    QVERIFY(visibleAnswer->viewport()->height() >=
            visibleAnswer->fontMetrics().height());
}

void AssistantResponseTest::showsInlineToolApprovalAndRedactsSecrets()
{
    ui::ToolApprovalWidget approval(
        infrastructure::mcp::ToolRisk::Destructive,
        QStringLiteral("filesystem.delete_file"),
        {{QStringLiteral("path"), QStringLiteral("D:/safe/file.txt")},
         {QStringLiteral("apiToken"), QStringLiteral("do-not-display")},
         {QStringLiteral("nested"),
          QJsonObject{
              {QStringLiteral("password"), QStringLiteral("also-hidden")}}}});
    approval.show();
    QSignalSpy decisionSpy(&approval, &ui::ToolApprovalWidget::decisionMade);

    auto* arguments = approval.findChild<QPlainTextEdit*>(
        QStringLiteral("toolApprovalArguments"));
    auto* allow =
        approval.findChild<QPushButton*>(QStringLiteral("allowToolButton"));
    auto* alwaysAllow = approval.findChild<QPushButton*>(
        QStringLiteral("alwaysAllowToolButton"));
    QVERIFY(arguments != nullptr);
    QVERIFY(allow != nullptr);
    QVERIFY(alwaysAllow != nullptr);
    QVERIFY(
        arguments->toPlainText().contains(QStringLiteral("D:/safe/file.txt")));
    QVERIFY(arguments->toPlainText().contains(QStringLiteral("[redacted]")));
    QVERIFY(
        !arguments->toPlainText().contains(QStringLiteral("do-not-display")));
    QVERIFY(!arguments->toPlainText().contains(QStringLiteral("also-hidden")));

    allow->click();
    QCOMPARE(decisionSpy.count(), 1);
    QCOMPARE(qvariant_cast<ui::ToolApprovalDecision>(
                 decisionSpy.constFirst().constFirst()),
             ui::ToolApprovalDecision::AllowOnce);
    QVERIFY(!allow->isVisible());

    ui::ToolApprovalWidget persistentApproval(
        infrastructure::mcp::ToolRisk::ModifiesData,
        QStringLiteral("filesystem.write_file"), {});
    QSignalSpy persistentSpy(&persistentApproval,
                             &ui::ToolApprovalWidget::decisionMade);
    auto* persistentButton = persistentApproval.findChild<QPushButton*>(
        QStringLiteral("alwaysAllowToolButton"));
    QVERIFY(persistentButton != nullptr);
    persistentButton->click();
    QCOMPARE(persistentSpy.count(), 1);
    QCOMPARE(qvariant_cast<ui::ToolApprovalDecision>(
                 persistentSpy.constFirst().constFirst()),
             ui::ToolApprovalDecision::AlwaysAllow);
}

void AssistantResponseTest::showsAgentActivityUntilRunEnds()
{
    ui::MainWindow window;
    auto* controller = window.findChild<application::AgentController*>();
    QVERIFY(controller != nullptr);

    emit controller->userRequestAccepted(QStringLiteral("run-id"),
                                         QStringLiteral("hello"));
    auto activityMessages = window.findChildren<ui::MessageWidget*>();
    QCOMPARE(activityMessages.size(), 2);
    auto* activity = static_cast<ui::MessageWidget*>(nullptr);
    for (auto* message : activityMessages)
    {
        if (message->property("agentActivity").toBool())
        {
            activity = message;
            break;
        }
    }
    QVERIFY(activity != nullptr);
    auto* body = activity->findChild<QTextBrowser*>(
        QStringLiteral("assistantMessageBody"));
    QVERIFY(body != nullptr);
    QCOMPARE(body->toPlainText(), QStringLiteral("Thinking."));
    QVERIFY(QMetaObject::invokeMethod(&window, "advanceThinkingAnimation",
                                      Qt::DirectConnection));
    QCOMPARE(body->toPlainText(), QStringLiteral("Thinking.."));

    emit controller->eventRecorded(
        {QStringLiteral("run-id"),
         agent::EventType::Warning,
         QStringLiteral("Still running; select Stop to cancel."),
         {},
         {}});
    QCOMPARE(body->toPlainText(),
             QStringLiteral("Still running; select Stop to cancel."));

    emit controller->stateChanged(application::AgentRun::State::ExecutingTool);
    QCOMPARE(body->toPlainText(), QStringLiteral("Using a tool..."));

    emit controller->finalAnswerReady(QStringLiteral("run-id"),
                                      QStringLiteral("Done."));
    activityMessages = window.findChildren<ui::MessageWidget*>();
    QCOMPARE(activityMessages.size(), 2);
    for (auto* message : activityMessages)
        QVERIFY(!message->property("agentActivity").toBool());

    emit controller->userRequestAccepted(QStringLiteral("failed-run"),
                                         QStringLiteral("try again"));
    QCOMPARE(window.findChildren<ui::MessageWidget*>().size(), 4);
    emit controller->stateChanged(application::AgentRun::State::Failed);
    activityMessages = window.findChildren<ui::MessageWidget*>();
    QCOMPARE(activityMessages.size(), 3);
    for (auto* message : activityMessages)
        QVERIFY(!message->property("agentActivity").toBool());
}

void AssistantResponseTest::recordsRedactedAgentActivity()
{
    ui::MainWindow window;
    auto* controller = window.findChild<application::AgentController*>();
    auto* activityLog =
        window.findChild<QPlainTextEdit*>(QStringLiteral("activityLog"));
    QVERIFY(controller != nullptr);
    QVERIFY(activityLog != nullptr);

    emit controller->eventRecorded(
        {QStringLiteral("run-id"),
         agent::EventType::RunStarted,
         QStringLiteral("Agent run started."),
         {},
         {},
         QDateTime::fromString(QStringLiteral("2026-08-14T02:03:04Z"),
                               Qt::ISODate)});
    emit controller->userRequestAccepted(QStringLiteral("run-id"),
                                         QStringLiteral("hello"));
    emit controller->eventRecorded(
        {QStringLiteral("run-id"),
         agent::EventType::ToolStarted,
         QStringLiteral("Tool call started."),
         QStringLiteral("filesystem.list_directory"),
         {{QStringLiteral("path"), QStringLiteral("D:/")},
          {QStringLiteral("apiToken"), QStringLiteral("do-not-display")}},
         QDateTime::fromString(QStringLiteral("2026-08-14T02:03:05Z"),
                               Qt::ISODate)});

    const auto activity = activityLog->toPlainText();
    QVERIFY(activity.contains(QStringLiteral("====================")));
    QVERIFY(activity.contains(QStringLiteral("Agent run started.")));
    QVERIFY(
        activity.contains(QStringLiteral("Tool: filesystem.list_directory")));
    QVERIFY(activity.contains(QStringLiteral("D:/")));
    QVERIFY(activity.contains(QStringLiteral("[redacted]")));
    QVERIFY(!activity.contains(QStringLiteral("do-not-display")));
    QVERIFY(!activity.contains(QStringLiteral("hello")));
}

void AssistantResponseTest::enterSendsAndShiftEnterAddsNewline()
{
    ui::MainWindow window;
    auto* prompt =
        window.findChild<QPlainTextEdit*>(QStringLiteral("promptEditor"));
    auto* send =
        window.findChild<QPushButton*>(QStringLiteral("primaryActionButton"));
    QVERIFY(prompt != nullptr);
    QVERIFY(send != nullptr);

    prompt->setEnabled(true);
    send->setEnabled(true);
    prompt->setPlainText(QStringLiteral("first line"));
    prompt->moveCursor(QTextCursor::End);

    QTest::keyClick(prompt, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(prompt->toPlainText(), QStringLiteral("first line\n"));
    QCOMPARE(window.findChildren<ui::MessageWidget*>().size(), 0);

    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(prompt->toPlainText(), QStringLiteral("first line"));
    QCOMPARE(window.findChildren<ui::MessageWidget*>().size(), 1);
}

void AssistantResponseTest::restoresPromptAfterGenerationError()
{
    ui::MainWindow window;
    auto* prompt =
        window.findChild<QPlainTextEdit*>(QStringLiteral("promptEditor"));
    QVERIFY(prompt != nullptr);

    const auto originalPrompt = QStringLiteral("keep this input for retry");
    prompt->setPlainText(originalPrompt);
    QVERIFY(QMetaObject::invokeMethod(&window, "sendPrompt"));
    QCOMPARE(prompt->toPlainText(), originalPrompt);
}

void AssistantResponseTest::clearsVisibleAndInMemoryConversation()
{
    ui::MainWindow window;
    auto* prompt =
        window.findChild<QPlainTextEdit*>(QStringLiteral("promptEditor"));
    auto* activityLog =
        window.findChild<QPlainTextEdit*>(QStringLiteral("activityLog"));
    auto* clearButton = window.findChild<QToolButton*>(
        QStringLiteral("clearConversationButton"));
    auto* transcriptTabs =
        window.findChild<QTabWidget*>(QStringLiteral("transcriptTabs"));
    auto* autoHideTabs = qobject_cast<ui::AutoHideTabWidget*>(transcriptTabs);
    auto* transcriptActions =
        window.findChild<QWidget*>(QStringLiteral("transcriptActions"));
    QVERIFY(prompt != nullptr);
    QVERIFY(activityLog != nullptr);
    QVERIFY(clearButton != nullptr);
    QVERIFY(transcriptTabs != nullptr);
    QVERIFY(autoHideTabs != nullptr);
    QVERIFY(transcriptActions != nullptr);
    QCOMPARE(transcriptTabs->tabPosition(), QTabWidget::South);
    QVERIFY(autoHideTabs->tabBar()->isHidden());
    QEnterEvent enterEvent(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QCoreApplication::sendEvent(autoHideTabs, &enterEvent);
    QVERIFY(!autoHideTabs->tabBar()->isHidden());
    QEvent leaveEvent(QEvent::Leave);
    QCoreApplication::sendEvent(autoHideTabs, &leaveEvent);
    QVERIFY(autoHideTabs->tabBar()->isHidden());
    QCOMPARE(clearButton->parentWidget(), transcriptActions);
    QVERIFY(clearButton->text().isEmpty());
    QVERIFY(!clearButton->toolTip().isEmpty());
    QVERIFY(!clearButton->isEnabled());

    prompt->setPlainText(QStringLiteral("temporary message"));
    QVERIFY(QMetaObject::invokeMethod(&window, "sendPrompt"));
    QCOMPARE(window.findChildren<ui::MessageWidget*>().size(), 1);
    QVERIFY(clearButton->isEnabled());

    clearButton->click();
    QCOMPARE(window.findChildren<ui::MessageWidget*>().size(), 0);
    QVERIFY(!clearButton->isEnabled());
    QVERIFY(activityLog->toPlainText().isEmpty());
}

void AssistantResponseTest::showsWorkspaceAsComposerLink()
{
    ui::MainWindow window;
    auto* composer =
        window.findChild<QWidget*>(QStringLiteral("promptComposer"));
    auto* workspaceLink =
        window.findChild<QLabel*>(QStringLiteral("workspacePathLink"));
    auto* workspaceButton =
        window.findChild<QToolButton*>(QStringLiteral("workspaceBrowseButton"));

    QVERIFY(composer != nullptr);
    QVERIFY(workspaceLink != nullptr);
    QVERIFY(workspaceButton != nullptr);
    QCOMPARE(workspaceLink->parentWidget(), composer);
    QCOMPARE(workspaceButton->parentWidget(), composer);
    QVERIFY(workspaceLink->text().contains(QStringLiteral("<a ")));
    QVERIFY(window.findChild<QLineEdit*>(QStringLiteral("workspacePathEdit")) ==
            nullptr);
}

void AssistantResponseTest::separatesModelLocationAndReloadActions()
{
    ui::ChatView view;
    view.setModelPresentation(QDir::homePath(), QStringLiteral("Test model"),
                              ui::ModelBadgeState::Verified);
    auto* modelLink = view.findChild<QLabel*>(QStringLiteral("modelPathLink"));
    auto* guideModelName =
        view.findChild<QLabel*>(QStringLiteral("guideModelNameLabel"));
    auto* reloadButton =
        view.findChild<QToolButton*>(QStringLiteral("modelReloadButton"));
    QVERIFY(modelLink != nullptr);
    QVERIFY(guideModelName != nullptr);
    QVERIFY(reloadButton != nullptr);
    QCOMPARE(modelLink->property("modelState").toString(),
             QStringLiteral("verified"));
    QCOMPARE(guideModelName->property("modelState").toString(),
             QStringLiteral("verified"));

    view.setModelPresentation(QDir::homePath(), QStringLiteral("Test model"),
                              ui::ModelBadgeState::Unverified);
    QCOMPARE(modelLink->property("modelState").toString(),
             QStringLiteral("unverified"));

    QSignalSpy locationRequested(&view, &ui::ChatView::modelLocationRequested);
    QSignalSpy folderRequested(&view, &ui::ChatView::modelFolderRequested);
    QVERIFY(QMetaObject::invokeMethod(
        modelLink, "linkActivated", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("open-model-location"))));
    QCOMPARE(locationRequested.count(), 1);
    QCOMPARE(folderRequested.count(), 0);

    reloadButton->click();
    QCOMPARE(locationRequested.count(), 1);
    QCOMPARE(folderRequested.count(), 1);
}

void AssistantResponseTest::
    placesModelControlsInComposerAndMergesPrimaryAction()
{
    ui::MainWindow window;
    auto* chatView = qobject_cast<ui::ChatView*>(window.centralWidget());
    auto* composer =
        window.findChild<QWidget*>(QStringLiteral("promptComposer"));
    auto* modelLink =
        window.findChild<QLabel*>(QStringLiteral("modelPathLink"));
    auto* modelReloadButton =
        window.findChild<QToolButton*>(QStringLiteral("modelReloadButton"));
    auto* guideLoadButton =
        window.findChild<QPushButton*>(QStringLiteral("guideLoadModelButton"));
    auto* guideStatus =
        window.findChild<QLabel*>(QStringLiteral("guideStatusLabel"));
    auto* statusLabel =
        window.findChild<QLabel*>(QStringLiteral("statusLabel"));
    auto* primaryAction =
        window.findChild<QPushButton*>(QStringLiteral("primaryActionButton"));
    auto* contentStack =
        window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* transcriptPage =
        window.findChild<QWidget*>(QStringLiteral("transcriptPage"));
    auto* transcriptTabs =
        window.findChild<QTabWidget*>(QStringLiteral("transcriptTabs"));
    auto* transcriptActions =
        window.findChild<QWidget*>(QStringLiteral("transcriptActions"));
    auto* rootLayout =
        window.findChild<QVBoxLayout*>(QStringLiteral("rootLayout"));

    QVERIFY(chatView != nullptr);
    QCOMPARE(window.centralWidget(), chatView);
    QVERIFY(composer != nullptr);
    QVERIFY(modelLink != nullptr);
    QVERIFY(modelReloadButton != nullptr);
    QVERIFY(guideLoadButton != nullptr);
    QVERIFY(guideStatus != nullptr);
    QVERIFY(statusLabel != nullptr);
    QVERIFY(primaryAction != nullptr);
    QVERIFY(contentStack != nullptr);
    QVERIFY(transcriptPage != nullptr);
    QVERIFY(transcriptTabs != nullptr);
    QVERIFY(transcriptActions != nullptr);
    QVERIFY(rootLayout != nullptr);
    QCOMPARE(modelLink->parentWidget(), composer);
    QCOMPARE(modelReloadButton->parentWidget(), transcriptActions);
    QCOMPARE(transcriptTabs->tabPosition(), QTabWidget::South);
    QCOMPARE(primaryAction->parentWidget(), composer);
    QCOMPARE(rootLayout->stretch(0), 1);
    QCOMPARE(rootLayout->stretch(1), 0);
    QCOMPARE(composer->sizePolicy().verticalPolicy(), QSizePolicy::Maximum);
    QCOMPARE(composer->maximumHeight(), 150);
    QVERIFY(!primaryAction->icon().isNull());
    QVERIFY(modelLink->text().contains(QStringLiteral("<a ")));
    QVERIFY(!modelLink->toolTip().isEmpty());
    QVERIFY(modelReloadButton->menu() == nullptr);
    QVERIFY(modelReloadButton->text().isEmpty());
    QVERIFY(!modelReloadButton->toolTip().isEmpty());
    QCOMPARE(guideLoadButton->text(), QStringLiteral("Load model"));
    QCOMPARE(contentStack->currentWidget()->objectName(),
             QStringLiteral("modelGuidePage"));
    QVERIFY(transcriptPage->isHidden());
    QVERIFY(composer->isHidden());
    QVERIFY(statusLabel->isHidden());
    QVERIFY(primaryAction->text().isEmpty());
    QVERIFY(!primaryAction->property("stopMode").toBool());
    QVERIFY(window.findChild<QWidget*>(QStringLiteral("modelBar")) == nullptr);
    QVERIFY(window.findChild<QLineEdit*>(QStringLiteral("modelPathEdit")) ==
            nullptr);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("modelInfoLabel")) ==
            nullptr);
    QVERIFY(window.findChild<QToolButton*>(
                QStringLiteral("modelBrowseButton")) == nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("loadModelButton")) ==
            nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("stopButton")) ==
            nullptr);

    QVERIFY(QMetaObject::invokeMethod(
        &window, "updateState", Qt::DirectConnection,
        Q_ARG(qtllm::infrastructure::WorkerClient::State,
              qtllm::infrastructure::WorkerClient::State::Ready)));
    QVERIFY(guideStatus->text().isEmpty());
    QVERIFY(guideStatus->isHidden());

    QVERIFY(QMetaObject::invokeMethod(
        &window, "updateState", Qt::DirectConnection,
        Q_ARG(qtllm::infrastructure::WorkerClient::State,
              qtllm::infrastructure::WorkerClient::State::Generating)));
    QVERIFY(primaryAction->property("stopMode").toBool());
    QCOMPARE(primaryAction->toolTip(), QStringLiteral("Stop generation"));
    QCOMPARE(contentStack->currentWidget()->objectName(),
             QStringLiteral("transcriptPage"));
    QVERIFY(!transcriptPage->isHidden());
    QVERIFY(!composer->isHidden());
    QVERIFY(!statusLabel->isHidden());

    QVERIFY(QMetaObject::invokeMethod(
        &window, "updateState", Qt::DirectConnection,
        Q_ARG(qtllm::infrastructure::WorkerClient::State,
              qtllm::infrastructure::WorkerClient::State::UnloadingModel)));
    QCOMPARE(guideStatus->text(), QStringLiteral("Releasing current model..."));
    QVERIFY(!guideStatus->isHidden());

    QVERIFY(QMetaObject::invokeMethod(
        &window, "updateState", Qt::DirectConnection,
        Q_ARG(qtllm::infrastructure::WorkerClient::State,
              qtllm::infrastructure::WorkerClient::State::LoadingModel)));
    QCOMPARE(guideStatus->text(), QStringLiteral("Loading model..."));
}
}  // namespace qtllm::tests

QTEST_MAIN(qtllm::tests::AssistantResponseTest)

#include "AssistantResponseTest.moc"
