#include "AssistantResponse.hpp"
#include "AutoHideTabWidget.hpp"
#include "ChatController.hpp"
#include "ChatView.hpp"
#include "ComputeSettingsDialog.hpp"
#include "MainWindow.hpp"
#include "McpControlPanel.hpp"
#include "McpServerDialog.hpp"
#include "MessageWidget.hpp"
#include "ModeSwitch.hpp"
#include "SensitiveData.hpp"
#include "Theme.hpp"
#include "ToolApprovalWidget.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextFormat>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtTest>

namespace qtllm::tests
{
class AssistantResponseTest final : public QObject
{
    Q_OBJECT

   private slots:
    void initTestCase();
    void init();
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
    void leavesPromptEmptyAfterGenerationError();
    void clearsVisibleAndInMemoryConversation();
    void showsWorkspaceAsComposerLink();
    void managesMcpServersFromAgentMenu();
    void presentsMcpServerControlAndSignals();
    void redactsSensitiveMcpDiagnostics();
    void validatesNewMcpServerConfiguration();
    void separatesModelLocationAndReloadActions();
    void presentsComputeSettingsOnStartPage();
    void placesModelControlsInComposerAndMergesPrimaryAction();
    void configuresSingleAndCustomGpuPlacement();

   private:
    [[nodiscard]] QString settingsFilePath() const;

    QTemporaryDir settingsDirectory_;
};

void AssistantResponseTest::initTestCase()
{
    QVERIFY(settingsDirectory_.isValid());
    ui::applyApplicationTheme(*qApp);
}

void AssistantResponseTest::init()
{
    const auto settingsPath = settingsFilePath();
    QVERIFY(!QFileInfo::exists(settingsPath) || QFile::remove(settingsPath));
    const auto mcpConfigPath =
        settingsDirectory_.filePath(QStringLiteral("mcp-servers.json"));
    QVERIFY(!QFileInfo::exists(mcpConfigPath) || QFile::remove(mcpConfigPath));
}

QString AssistantResponseTest::settingsFilePath() const
{
    return settingsDirectory_.filePath(QStringLiteral("qtLLM.ini"));
}

void AssistantResponseTest::configuresSingleAndCustomGpuPlacement()
{
    const QList<inference::ComputeDevice> devices{
        {QStringLiteral("0000:02:00.0"), QStringLiteral("CUDA0"),
         QStringLiteral("NVIDIA GeForce RTX 3090"),
         QStringLiteral("0000:02:00.0"), 20ULL * 1024 * 1024 * 1024,
         24ULL * 1024 * 1024 * 1024},
        {QStringLiteral("0000:83:00.0"), QStringLiteral("CUDA1"),
         QStringLiteral("NVIDIA GeForce RTX 3090"),
         QStringLiteral("0000:83:00.0"), 21ULL * 1024 * 1024 * 1024,
         24ULL * 1024 * 1024 * 1024}};
    ui::ComputeSettingsDialog dialog(devices);
    auto* mode =
        dialog.findChild<QComboBox*>(QStringLiteral("placementModeCombo"));
    auto* table =
        dialog.findChild<QTableWidget*>(QStringLiteral("deviceTable"));
    QVERIFY(mode != nullptr);
    QVERIFY(table != nullptr);

    mode->setCurrentIndex(mode->findData(
        static_cast<int>(inference::DevicePlacementMode::Single)));
    QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
    QCOMPARE(table->item(1, 0)->checkState(), Qt::Unchecked);
    table->item(1, 0)->setCheckState(Qt::Checked);
    QCOMPARE(table->item(0, 0)->checkState(), Qt::Unchecked);
    QCOMPARE(table->item(1, 0)->checkState(), Qt::Checked);

    mode->setCurrentIndex(mode->findData(
        static_cast<int>(inference::DevicePlacementMode::Custom)));
    QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
    QCOMPARE(table->item(1, 0)->checkState(), Qt::Checked);
    auto* secondWeight = qobject_cast<QDoubleSpinBox*>(table->cellWidget(1, 2));
    QVERIFY(secondWeight != nullptr);
    secondWeight->setValue(2.5);
    dialog.accept();

    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    inference::ModelLoadOptions expected;
    expected.placementMode = inference::DevicePlacementMode::Custom;
    expected.devices = {{QStringLiteral("0000:02:00.0"), 1.0F},
                        {QStringLiteral("0000:83:00.0"), 2.5F}};
    QCOMPARE(dialog.options(), expected);
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
    auto* userAvatar = user.findChild<QLabel*>(QStringLiteral("userAvatar"));
    auto* assistantAvatar =
        assistant.findChild<QLabel*>(QStringLiteral("assistantAvatar"));
    QVERIFY(userBody != nullptr);
    QVERIFY(assistantBody != nullptr);
    QVERIFY(userAvatar != nullptr);
    QVERIFY(assistantAvatar != nullptr);
    QCOMPARE(userAvatar->text(), QStringLiteral("You"));
    QCOMPARE(assistantAvatar->text(), QStringLiteral("AI"));
    QCOMPARE(userAvatar->size(), QSize(36, 36));
    QCOMPARE(assistantAvatar->size(), QSize(36, 36));
    const auto userBodyCenter =
        userBody->mapTo(&user, userBody->rect().center()).x();
    const auto userAvatarCenter =
        userAvatar->mapTo(&user, userAvatar->rect().center()).x();
    const auto assistantBodyCenter =
        assistantBody->mapTo(&assistant, assistantBody->rect().center()).x();
    const auto assistantAvatarCenter =
        assistantAvatar->mapTo(&assistant, assistantAvatar->rect().center())
            .x();
    QVERIFY(userBody->width() <= user.width() * 7 / 10);
    QVERIFY(userBodyCenter > user.width() / 2);
    QVERIFY(userAvatarCenter > userBodyCenter);
    QVERIFY(assistantAvatarCenter < assistantBodyCenter);
    QVERIFY(assistantBody->width() >= 480);
    QVERIFY(assistantBody->width() <= assistant.width() * 84 / 100);
    QVERIFY(assistantBody->document()->defaultStyleSheet().contains(
        QStringLiteral("blockquote")));
    QVERIFY(assistantBody->document()->defaultStyleSheet().contains(
        QStringLiteral("pre")));

    ui::MessageWidget shortAssistant(ui::MessageWidget::Role::Assistant);
    shortAssistant.resize(900, 100);
    shortAssistant.setAssistantText(QStringLiteral("你好"), true);
    shortAssistant.show();
    QTest::qWait(20);
    auto* shortAssistantBody = shortAssistant.findChild<QTextBrowser*>(
        QStringLiteral("assistantMessageBody"));
    QVERIFY(shortAssistantBody != nullptr);
    QVERIFY(shortAssistantBody->width() <= 80);

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
    ui::MainWindow window(settingsFilePath());
    auto* controller = window.findChild<application::AgentController*>();
    auto* prompt =
        window.findChild<QPlainTextEdit*>(QStringLiteral("promptEditor"));
    QVERIFY(controller != nullptr);
    QVERIFY(prompt != nullptr);

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
    auto* thinkingTimer =
        window.findChild<QTimer*>(QStringLiteral("thinkingAnimationTimer"));
    QVERIFY(thinkingTimer != nullptr);
    QVERIFY(thinkingTimer->isActive());
    const auto thinkingTimerId = thinkingTimer->timerId();

    emit controller->stateChanged(application::AgentRun::State::Deciding);
    QVERIFY(thinkingTimer->isActive());
    QCOMPARE(thinkingTimer->timerId(), thinkingTimerId);
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
    emit controller->runFinished(QStringLiteral("failed-run"),
                                 application::AgentRun::State::Failed,
                                 QStringLiteral("generation_failed"),
                                 QStringLiteral("Generation failed."));
    QVERIFY(prompt->toPlainText().isEmpty());
}

void AssistantResponseTest::recordsRedactedAgentActivity()
{
    ui::MainWindow window(settingsFilePath());
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
    ui::MainWindow window(settingsFilePath());
    auto* chatView = qobject_cast<ui::ChatView*>(window.centralWidget());
    auto* chatController = window.findChild<application::ChatController*>();
    auto* prompt =
        window.findChild<QPlainTextEdit*>(QStringLiteral("promptEditor"));
    auto* send =
        window.findChild<QPushButton*>(QStringLiteral("primaryActionButton"));
    QVERIFY(chatView != nullptr);
    QVERIFY(chatController != nullptr);
    QVERIFY(prompt != nullptr);
    QVERIFY(send != nullptr);
    chatView->setAgentModeSelected(false);
    QSignalSpy chatRequestSpy(
        chatController, &application::ChatController::userMessageAccepted);

    prompt->setEnabled(true);
    send->setEnabled(true);
    prompt->setPlainText(QStringLiteral("first line"));
    prompt->moveCursor(QTextCursor::End);

    QTest::keyClick(prompt, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(prompt->toPlainText(), QStringLiteral("first line\n"));
    QCOMPARE(window.findChildren<ui::MessageWidget*>().size(), 0);

    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(chatRequestSpy.count(), 1);
    QVERIFY(prompt->toPlainText().isEmpty());
    QCOMPARE(window.findChildren<ui::MessageWidget*>().size(), 2);
}

void AssistantResponseTest::leavesPromptEmptyAfterGenerationError()
{
    ui::MainWindow window(settingsFilePath());
    auto* chatView = qobject_cast<ui::ChatView*>(window.centralWidget());
    auto* prompt =
        window.findChild<QPlainTextEdit*>(QStringLiteral("promptEditor"));
    QVERIFY(chatView != nullptr);
    QVERIFY(prompt != nullptr);

    chatView->setAgentModeSelected(false);
    const auto originalPrompt = QStringLiteral("do not restore this input");
    prompt->setPlainText(originalPrompt);
    QVERIFY(QMetaObject::invokeMethod(&window, "sendPrompt"));
    QVERIFY(prompt->toPlainText().isEmpty());
}

void AssistantResponseTest::clearsVisibleAndInMemoryConversation()
{
    ui::MainWindow window(settingsFilePath());
    auto* chatView = qobject_cast<ui::ChatView*>(window.centralWidget());
    auto* prompt =
        window.findChild<QPlainTextEdit*>(QStringLiteral("promptEditor"));
    auto* activityLog =
        window.findChild<QPlainTextEdit*>(QStringLiteral("activityLog"));
    auto* clearAction =
        window.findChild<QAction*>(QStringLiteral("clearConversationAction"));
    auto* transcriptTabs =
        window.findChild<QTabWidget*>(QStringLiteral("transcriptTabs"));
    auto* autoHideTabs = qobject_cast<ui::AutoHideTabWidget*>(transcriptTabs);
    QVERIFY(chatView != nullptr);
    QVERIFY(prompt != nullptr);
    QVERIFY(activityLog != nullptr);
    QVERIFY(clearAction != nullptr);
    QVERIFY(transcriptTabs != nullptr);
    QVERIFY(autoHideTabs != nullptr);
    QCOMPARE(transcriptTabs->tabPosition(), QTabWidget::South);
    QCOMPARE(transcriptTabs->contextMenuPolicy(), Qt::ActionsContextMenu);
    QVERIFY(transcriptTabs->actions().contains(clearAction));
    QVERIFY(autoHideTabs->tabBar()->isHidden());
    QEnterEvent enterEvent(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QCoreApplication::sendEvent(autoHideTabs, &enterEvent);
    QVERIFY(!autoHideTabs->tabBar()->isHidden());
    QEvent leaveEvent(QEvent::Leave);
    QCoreApplication::sendEvent(autoHideTabs, &leaveEvent);
    QVERIFY(autoHideTabs->tabBar()->isHidden());
    QCOMPARE(clearAction->text(), QStringLiteral("Clear conversation"));
    QVERIFY(!clearAction->toolTip().isEmpty());
    QVERIFY(!clearAction->isEnabled());

    chatView->setAgentModeSelected(false);
    prompt->setPlainText(QStringLiteral("temporary message"));
    QVERIFY(QMetaObject::invokeMethod(&window, "sendPrompt"));
    QCOMPARE(window.findChildren<ui::MessageWidget*>().size(), 2);
    QVERIFY(clearAction->isEnabled());

    clearAction->trigger();
    QCOMPARE(window.findChildren<ui::MessageWidget*>().size(), 0);
    QVERIFY(!clearAction->isEnabled());
    QVERIFY(activityLog->toPlainText().isEmpty());
}

void AssistantResponseTest::showsWorkspaceAsComposerLink()
{
    ui::MainWindow window(settingsFilePath());
    auto* chatView = qobject_cast<ui::ChatView*>(window.centralWidget());
    auto* composer =
        window.findChild<QWidget*>(QStringLiteral("promptComposer"));
    auto* workspaceLink =
        window.findChild<QLabel*>(QStringLiteral("workspacePathLink"));
    auto* workspaceChangeAction =
        window.findChild<QAction*>(QStringLiteral("workspaceChangeAction"));

    QVERIFY(chatView != nullptr);
    QVERIFY(composer != nullptr);
    QVERIFY(workspaceLink != nullptr);
    QVERIFY(workspaceChangeAction != nullptr);
    QCOMPARE(workspaceLink->parentWidget(), composer);
    QCOMPARE(workspaceLink->contextMenuPolicy(), Qt::ActionsContextMenu);
    QVERIFY(workspaceLink->actions().contains(workspaceChangeAction));
    QVERIFY(!workspaceLink->text().contains(QStringLiteral("<a ")));
    QVERIFY(workspaceLink->width() <= 180);
    QVERIFY(!workspaceLink->toolTip().isEmpty());
    QVERIFY(!workspaceLink->accessibleDescription().isEmpty());
    QVERIFY(window.findChild<QToolButton*>(
                QStringLiteral("workspaceBrowseButton")) == nullptr);
    QVERIFY(window.findChild<QLineEdit*>(QStringLiteral("workspacePathEdit")) ==
            nullptr);

    QVERIFY(QObject::disconnect(chatView, nullptr, &window, nullptr));
    QSignalSpy openRequested(chatView, &ui::ChatView::workspaceOpenRequested);
    QSignalSpy changeRequested(chatView,
                               &ui::ChatView::workspaceFolderRequested);
    QTest::mouseClick(workspaceLink, Qt::LeftButton);
    QCOMPARE(openRequested.count(), 1);
    QCOMPARE(changeRequested.count(), 0);
    workspaceChangeAction->trigger();
    QCOMPARE(openRequested.count(), 1);
    QCOMPARE(changeRequested.count(), 1);
    chatView->setWorkspaceControlsEnabled(false);
    QVERIFY(workspaceLink->isEnabled());
    QVERIFY(!workspaceChangeAction->isEnabled());
}

void AssistantResponseTest::managesMcpServersFromAgentMenu()
{
    ui::ChatView view;
    auto* button =
        view.findChild<QToolButton*>(QStringLiteral("mcpMenuButton"));
    auto* menu = view.findChild<QMenu*>(QStringLiteral("mcpServerMenu"));
    QVERIFY(button != nullptr);
    QVERIFY(menu != nullptr);
    QVERIFY(button->isHidden());

    view.setMcpServers({{QStringLiteral("builtin-filesystem"),
                         QStringLiteral("Built-in filesystem"),
                         QStringLiteral("7 tools"), true, true, true},
                        {QStringLiteral("external"), QStringLiteral("external"),
                         QStringLiteral("Off"), false, false, true}});
    view.setAgentModeSelected(true);
    QVERIFY(!button->isHidden());
    QCOMPARE(button->popupMode(), QToolButton::InstantPopup);
    QCOMPARE(button->menu(), menu);

    auto* builtInAction = menu->findChild<QAction*>(
        QStringLiteral("mcpServerAction_builtin-filesystem"));
    auto* externalAction =
        menu->findChild<QAction*>(QStringLiteral("mcpServerAction_external"));
    auto* addAction =
        menu->findChild<QAction*>(QStringLiteral("addMcpServerAction"));
    auto* manageAction =
        menu->findChild<QAction*>(QStringLiteral("manageMcpServersAction"));
    auto* removeMenu =
        menu->findChild<QMenu*>(QStringLiteral("removeMcpServerMenu"));
    auto* removeExternalAction = menu->findChild<QAction*>(
        QStringLiteral("removeMcpServerAction_external"));
    auto* removeBuiltInAction = menu->findChild<QAction*>(
        QStringLiteral("removeMcpServerAction_builtin-filesystem"));
    QVERIFY(builtInAction != nullptr);
    QVERIFY(externalAction != nullptr);
    QVERIFY(addAction != nullptr);
    QVERIFY(manageAction != nullptr);
    QVERIFY(removeMenu != nullptr);
    QVERIFY(removeExternalAction != nullptr);
    QVERIFY(removeBuiltInAction == nullptr);
    QVERIFY(builtInAction->isCheckable());
    QVERIFY(builtInAction->isChecked());
    QVERIFY(!externalAction->isChecked());

    QSignalSpy builtInSpy(&view, &ui::ChatView::builtInFilesystemMcpToggled);
    QSignalSpy externalSpy(&view, &ui::ChatView::externalMcpServerToggled);
    QSignalSpy addSpy(&view, &ui::ChatView::addMcpServerRequested);
    QSignalSpy manageSpy(&view, &ui::ChatView::manageMcpServersRequested);
    QSignalSpy removeSpy(&view,
                         &ui::ChatView::removeExternalMcpServerRequested);
    builtInAction->trigger();
    QTRY_COMPARE(builtInSpy.count(), 1);
    QCOMPARE(builtInSpy.constFirst().constFirst().toBool(), false);
    externalAction->trigger();
    QTRY_COMPARE(externalSpy.count(), 1);
    QCOMPARE(externalSpy.constFirst().at(0).toString(),
             QStringLiteral("external"));
    QCOMPARE(externalSpy.constFirst().at(1).toBool(), true);
    addAction->trigger();
    QCOMPARE(addSpy.count(), 1);
    manageAction->trigger();
    QCOMPARE(manageSpy.count(), 1);
    removeExternalAction->trigger();
    QCOMPARE(removeSpy.count(), 1);
    QCOMPARE(removeSpy.constFirst().constFirst().toString(),
             QStringLiteral("external"));

    view.setAgentModeSelected(false);
    QVERIFY(button->isHidden());
}

void AssistantResponseTest::presentsMcpServerControlAndSignals()
{
    using infrastructure::mcp::McpServerState;

    ui::McpServerControlPresentation server;
    server.serverId = QStringLiteral("external");
    server.displayName = QStringLiteral("Example MCP");
    server.enabled = true;
    server.available = true;
    server.registered = true;
    server.controlsEnabled = true;
    server.snapshot.serverId = server.serverId;
    server.snapshot.state = McpServerState::Ready;
    server.snapshot.toolCount = 1;
    server.snapshot.protocolVersion = QStringLiteral("2025-06-18");
    server.snapshot.capabilities.tools = true;
    server.snapshot.capabilities.resources = true;
    server.snapshot.capabilities.resourcesSubscribe = true;
    server.snapshot.capabilities.prompts = true;
    server.snapshot.capabilities.logging = true;
    server.snapshot.capabilities.completions = true;
    server.snapshot.loggingLevel = QStringLiteral("warning");
    server.snapshot.instructions = QStringLiteral("Use bounded results.");
    server.snapshot.lastErrorCode = QStringLiteral("last_failure");
    server.snapshot.lastErrorMessage = QStringLiteral("Recovered safely.");
    server.transport = QStringLiteral("stdio");
    server.serverInformation = QStringLiteral("example 1.0");
    server.program = QStringLiteral("D:/tools/example-mcp.exe");
    server.workingDirectory = QStringLiteral("D:/workspace");
    server.configurationSummary = QStringLiteral("stdio; 2 arguments");
    server.allowlist = QStringLiteral("read_data");
    server.authorizedRoots = QStringLiteral("D:/workspace");
    server.loggingLevel = QStringLiteral("warning");
    server.appliedLoggingLevel = QStringLiteral("warning");
    server.useInstructions = true;
    server.instructionsSource = QStringLiteral("external (included)");
    server.tools.append({QStringLiteral("read_data"),
                         QStringLiteral("Allowed, read only"),
                         QStringLiteral("read only, idempotent")});
    server.resources.append({QStringLiteral("test://resource/readme"),
                             QStringLiteral("Readme"),
                             QStringLiteral("text/plain"), false});
    server.resources.append({QStringLiteral("test://resource/{name}"),
                             QStringLiteral("Named"), QString{}, true});
    server.prompts.append({QStringLiteral("summarize"),
                           QStringLiteral("Summarize a topic"),
                           {{QStringLiteral("topic"), QString{}, true}}});
    server.diagnostics.append(
        QStringLiteral("[12:00:00] Protocol: Initialized"));

    ui::McpControlPanel panel;
    panel.setServers({server});
    panel.show();
    QTest::qWait(20);

    auto* serverList =
        panel.findChild<QTreeWidget*>(QStringLiteral("mcpServerList"));
    auto* stateLabel =
        panel.findChild<QLabel*>(QStringLiteral("mcpServerState"));
    auto* enabled =
        panel.findChild<QCheckBox*>(QStringLiteral("mcpServerEnabled"));
    auto* instructions =
        panel.findChild<QPlainTextEdit*>(QStringLiteral("mcpInstructions"));
    auto* useInstructions =
        panel.findChild<QCheckBox*>(QStringLiteral("mcpUseInstructions"));
    auto* instructionsSource =
        panel.findChild<QLabel*>(QStringLiteral("mcpInstructionsSource"));
    auto* loggingLevel =
        panel.findChild<QComboBox*>(QStringLiteral("mcpLoggingLevel"));
    auto* loggingStatus =
        panel.findChild<QLabel*>(QStringLiteral("mcpLoggingStatus"));
    auto* lastError = panel.findChild<QLabel*>(QStringLiteral("mcpLastError"));
    auto* toolList =
        panel.findChild<QTreeWidget*>(QStringLiteral("mcpToolList"));
    auto* diagnostics =
        panel.findChild<QPlainTextEdit*>(QStringLiteral("mcpDiagnostics"));
    auto* resourceList =
        panel.findChild<QTreeWidget*>(QStringLiteral("mcpResourceList"));
    auto* resourceResult =
        panel.findChild<QPlainTextEdit*>(QStringLiteral("mcpResourceResult"));
    auto* readResource =
        panel.findChild<QToolButton*>(QStringLiteral("readMcpResourceButton"));
    auto* subscribeResource = panel.findChild<QToolButton*>(
        QStringLiteral("subscribeMcpResourceButton"));
    auto* resourceCompletionArgument = panel.findChild<QComboBox*>(
        QStringLiteral("mcpResourceCompletionArgument"));
    auto* resourceCompletionValue = panel.findChild<QLineEdit*>(
        QStringLiteral("mcpResourceCompletionValue"));
    auto* completeResource = panel.findChild<QToolButton*>(
        QStringLiteral("completeMcpResourceButton"));
    auto* resourceCompletionResult = panel.findChild<QPlainTextEdit*>(
        QStringLiteral("mcpResourceCompletionResult"));
    auto* promptList =
        panel.findChild<QTreeWidget*>(QStringLiteral("mcpPromptList"));
    auto* promptArguments =
        panel.findChild<QLineEdit*>(QStringLiteral("mcpPromptArguments"));
    auto* promptResult =
        panel.findChild<QPlainTextEdit*>(QStringLiteral("mcpPromptResult"));
    auto* getPrompt =
        panel.findChild<QToolButton*>(QStringLiteral("getMcpPromptButton"));
    auto* promptCompletionArgument = panel.findChild<QComboBox*>(
        QStringLiteral("mcpPromptCompletionArgument"));
    auto* promptCompletionValue =
        panel.findChild<QLineEdit*>(QStringLiteral("mcpPromptCompletionValue"));
    auto* completePrompt = panel.findChild<QToolButton*>(
        QStringLiteral("completeMcpPromptButton"));
    auto* promptCompletionResult = panel.findChild<QPlainTextEdit*>(
        QStringLiteral("mcpPromptCompletionResult"));
    auto* start =
        panel.findChild<QToolButton*>(QStringLiteral("startMcpServerButton"));
    auto* stop =
        panel.findChild<QToolButton*>(QStringLiteral("stopMcpServerButton"));
    auto* restart =
        panel.findChild<QToolButton*>(QStringLiteral("restartMcpServerButton"));
    auto* ping =
        panel.findChild<QToolButton*>(QStringLiteral("pingMcpServerButton"));
    auto* refresh =
        panel.findChild<QToolButton*>(QStringLiteral("refreshMcpToolsButton"));
    auto* add =
        panel.findChild<QToolButton*>(QStringLiteral("addMcpServerButton"));
    auto* remove =
        panel.findChild<QToolButton*>(QStringLiteral("removeMcpServerButton"));
    QVERIFY(serverList != nullptr);
    QVERIFY(stateLabel != nullptr);
    QVERIFY(enabled != nullptr);
    QVERIFY(instructions != nullptr);
    QVERIFY(useInstructions != nullptr);
    QVERIFY(instructionsSource != nullptr);
    QVERIFY(loggingLevel != nullptr);
    QVERIFY(loggingStatus != nullptr);
    QVERIFY(lastError != nullptr);
    QVERIFY(toolList != nullptr);
    QVERIFY(diagnostics != nullptr);
    QVERIFY(resourceList != nullptr);
    QVERIFY(resourceResult != nullptr);
    QVERIFY(readResource != nullptr);
    QVERIFY(subscribeResource != nullptr);
    QVERIFY(resourceCompletionArgument != nullptr);
    QVERIFY(resourceCompletionValue != nullptr);
    QVERIFY(completeResource != nullptr);
    QVERIFY(resourceCompletionResult != nullptr);
    QVERIFY(promptList != nullptr);
    QVERIFY(promptArguments != nullptr);
    QVERIFY(promptResult != nullptr);
    QVERIFY(getPrompt != nullptr);
    QVERIFY(promptCompletionArgument != nullptr);
    QVERIFY(promptCompletionValue != nullptr);
    QVERIFY(completePrompt != nullptr);
    QVERIFY(promptCompletionResult != nullptr);
    QVERIFY(start != nullptr);
    QVERIFY(stop != nullptr);
    QVERIFY(restart != nullptr);
    QVERIFY(ping != nullptr);
    QVERIFY(refresh != nullptr);
    QVERIFY(add != nullptr);
    QVERIFY(remove != nullptr);

    QCOMPARE(serverList->topLevelItemCount(), 1);
    QCOMPARE(serverList->topLevelItem(0)->text(0),
             QStringLiteral("Example MCP"));
    QCOMPARE(serverList->topLevelItem(0)->text(1), QStringLiteral("Ready"));
    QCOMPARE(serverList->topLevelItem(0)->text(2), QStringLiteral("1"));
    QCOMPARE(stateLabel->text(), QStringLiteral("Ready"));
    QCOMPARE(stateLabel->property("serverState").toString(),
             QStringLiteral("ready"));
    QVERIFY(enabled->isChecked());
    QCOMPARE(instructions->toPlainText(),
             QStringLiteral("Use bounded results."));
    QVERIFY(useInstructions->isChecked());
    QCOMPARE(instructionsSource->text(), QStringLiteral("external (included)"));
    QCOMPARE(loggingLevel->currentData().toString(), QStringLiteral("warning"));
    QCOMPARE(loggingStatus->text(), QStringLiteral("Applied: warning"));
    QCOMPARE(lastError->text(),
             QStringLiteral("last_failure: Recovered safely."));
    QCOMPARE(toolList->topLevelItemCount(), 1);
    QCOMPARE(toolList->topLevelItem(0)->text(0), QStringLiteral("read_data"));
    QVERIFY(diagnostics->toPlainText().contains(QStringLiteral("Initialized")));
    QCOMPARE(resourceList->topLevelItemCount(), 2);
    QCOMPARE(resourceList->topLevelItem(0)->text(0), QStringLiteral("Readme"));
    QVERIFY(readResource->isEnabled());
    QVERIFY(subscribeResource->isEnabled());
    QCOMPARE(promptList->topLevelItemCount(), 1);
    QCOMPARE(promptList->topLevelItem(0)->text(0), QStringLiteral("summarize"));
    QVERIFY(promptArguments->text().contains(QStringLiteral("topic")));
    QVERIFY(getPrompt->isEnabled());
    QCOMPARE(promptCompletionArgument->currentText(), QStringLiteral("topic"));
    QVERIFY(completePrompt->isEnabled());
    QVERIFY(!start->isEnabled());
    QVERIFY(stop->isEnabled());
    QVERIFY(restart->isEnabled());
    QVERIFY(ping->isEnabled());
    QVERIFY(refresh->isEnabled());
    QVERIFY(remove->isEnabled());

    QSignalSpy addSpy(&panel, &ui::McpControlPanel::addServerRequested);
    QSignalSpy removeSpy(&panel, &ui::McpControlPanel::removeServerRequested);
    QSignalSpy enabledSpy(&panel, &ui::McpControlPanel::serverEnabledChanged);
    QSignalSpy instructionsSpy(
        &panel, &ui::McpControlPanel::instructionsEnabledChanged);
    QSignalSpy loggingSpy(&panel, &ui::McpControlPanel::loggingLevelRequested);
    QSignalSpy startSpy(&panel, &ui::McpControlPanel::startServerRequested);
    QSignalSpy stopSpy(&panel, &ui::McpControlPanel::stopServerRequested);
    QSignalSpy restartSpy(&panel, &ui::McpControlPanel::restartServerRequested);
    QSignalSpy pingSpy(&panel, &ui::McpControlPanel::pingServerRequested);
    QSignalSpy refreshSpy(&panel, &ui::McpControlPanel::refreshToolsRequested);
    QSignalSpy readResourceSpy(&panel,
                               &ui::McpControlPanel::readResourceRequested);
    QSignalSpy subscriptionSpy(
        &panel, &ui::McpControlPanel::resourceSubscriptionRequested);
    QSignalSpy getPromptSpy(&panel, &ui::McpControlPanel::getPromptRequested);
    QSignalSpy completePromptSpy(&panel,
                                 &ui::McpControlPanel::completePromptRequested);
    QSignalSpy completeResourceSpy(
        &panel, &ui::McpControlPanel::completeResourceTemplateRequested);
    add->click();
    remove->click();
    stop->click();
    restart->click();
    ping->click();
    refresh->click();
    readResource->click();
    subscribeResource->click();
    resourceList->setCurrentItem(resourceList->topLevelItem(1));
    QCOMPARE(resourceCompletionArgument->currentText(), QStringLiteral("name"));
    resourceCompletionValue->setText(QStringLiteral("read"));
    QVERIFY(completeResource->isEnabled());
    completeResource->click();
    promptArguments->setText(QStringLiteral(R"({"topic":"MCP"})"));
    getPrompt->click();
    promptCompletionValue->setText(QStringLiteral("mc"));
    completePrompt->click();
    useInstructions->click();
    const auto infoIndex = loggingLevel->findData(QStringLiteral("info"));
    QVERIFY(infoIndex >= 0);
    loggingLevel->setCurrentIndex(infoIndex);
    QVERIFY(QMetaObject::invokeMethod(loggingLevel, "activated",
                                      Q_ARG(int, infoIndex)));
    enabled->click();
    QCOMPARE(addSpy.count(), 1);
    QCOMPARE(removeSpy.count(), 1);
    QCOMPARE(removeSpy.constFirst().constFirst().toString(), server.serverId);
    QCOMPARE(stopSpy.count(), 1);
    QCOMPARE(restartSpy.count(), 1);
    QCOMPARE(pingSpy.count(), 1);
    QCOMPARE(pingSpy.constFirst().constFirst().toString(), server.serverId);
    QCOMPARE(refreshSpy.count(), 1);
    QCOMPARE(readResourceSpy.count(), 1);
    QCOMPARE(readResourceSpy.constFirst().at(0).toString(), server.serverId);
    QCOMPARE(readResourceSpy.constFirst().at(1).toString(),
             QStringLiteral("test://resource/readme"));
    QCOMPARE(subscriptionSpy.count(), 1);
    QCOMPARE(subscriptionSpy.constFirst().at(2).toBool(), true);
    QCOMPARE(completeResourceSpy.count(), 1);
    QCOMPARE(completeResourceSpy.constFirst().at(1).toString(),
             QStringLiteral("test://resource/{name}"));
    QCOMPARE(completeResourceSpy.constFirst().at(2).toString(),
             QStringLiteral("name"));
    QCOMPARE(completeResourceSpy.constFirst().at(3).toString(),
             QStringLiteral("read"));
    QCOMPARE(getPromptSpy.count(), 1);
    QCOMPARE(getPromptSpy.constFirst().at(1).toString(),
             QStringLiteral("summarize"));
    QCOMPARE(getPromptSpy.constFirst()
                 .at(2)
                 .toJsonObject()
                 .value(QStringLiteral("topic"))
                 .toString(),
             QStringLiteral("MCP"));
    QCOMPARE(completePromptSpy.count(), 1);
    QCOMPARE(completePromptSpy.constFirst().at(1).toString(),
             QStringLiteral("summarize"));
    QCOMPARE(completePromptSpy.constFirst().at(2).toString(),
             QStringLiteral("topic"));
    QCOMPARE(completePromptSpy.constFirst().at(3).toString(),
             QStringLiteral("mc"));
    QCOMPARE(completePromptSpy.constFirst()
                 .at(4)
                 .toJsonObject()
                 .value(QStringLiteral("topic"))
                 .toString(),
             QStringLiteral("MCP"));
    QCOMPARE(instructionsSpy.count(), 1);
    QCOMPARE(instructionsSpy.constFirst().at(0).toString(), server.serverId);
    QCOMPARE(instructionsSpy.constFirst().at(1).toBool(), false);
    QCOMPARE(instructionsSpy.constFirst().at(2).toBool(), false);
    QCOMPARE(loggingSpy.count(), 1);
    QCOMPARE(loggingSpy.constFirst().at(0).toString(), server.serverId);
    QCOMPARE(loggingSpy.constFirst().at(1).toString(), QStringLiteral("info"));
    QCOMPARE(enabledSpy.count(), 1);
    QCOMPARE(enabledSpy.constFirst().at(0).toString(), server.serverId);
    QCOMPARE(enabledSpy.constFirst().at(1).toBool(), false);
    QCOMPARE(enabledSpy.constFirst().at(2).toBool(), false);

    infrastructure::mcp::McpResourceReadResult displayedResource;
    displayedResource.serverId = server.serverId;
    displayedResource.requestedUri = QStringLiteral("test://resource/readme");
    displayedResource.contents.append({displayedResource.requestedUri,
                                       QStringLiteral("text/plain"),
                                       QStringLiteral("Resource body"),
                                       {},
                                       false});
    panel.showResourceResult(displayedResource);
    QVERIFY(resourceResult->toPlainText().contains(
        QStringLiteral("Resource body")));

    infrastructure::mcp::McpPromptResult displayedPrompt;
    displayedPrompt.serverId = server.serverId;
    displayedPrompt.promptName = QStringLiteral("summarize");
    displayedPrompt.messages.append(
        {QStringLiteral("user"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("text")},
             {QStringLiteral("text"), QStringLiteral("Summarize MCP")}}});
    panel.showPromptResult(displayedPrompt);
    QVERIFY(
        promptResult->toPlainText().contains(QStringLiteral("Summarize MCP")));

    infrastructure::mcp::McpCompletionResult displayedCompletion;
    displayedCompletion.serverId = server.serverId;
    displayedCompletion.referenceType = QStringLiteral("ref/prompt");
    displayedCompletion.values = {QStringLiteral("mcp"),
                                  QStringLiteral("mcp host")};
    displayedCompletion.total = 3;
    displayedCompletion.totalProvided = true;
    displayedCompletion.hasMore = true;
    displayedCompletion.hasMoreProvided = true;
    panel.showCompletionResult(displayedCompletion);
    QVERIFY(promptCompletionResult->toPlainText().contains(
        QStringLiteral("mcp host")));
    QVERIFY(promptCompletionResult->toPlainText().contains(
        QStringLiteral("more available")));
    displayedCompletion.referenceType = QStringLiteral("ref/resource");
    panel.showCompletionResult(displayedCompletion);
    QVERIFY(resourceCompletionResult->toPlainText().contains(
        QStringLiteral("mcp host")));

    server.snapshot.state = McpServerState::Stopped;
    panel.setServers({server});
    QVERIFY(start->isEnabled());
    start->click();
    QCOMPARE(startSpy.count(), 1);
    QCOMPARE(startSpy.constFirst().constFirst().toString(), server.serverId);
}

void AssistantResponseTest::redactsSensitiveMcpDiagnostics()
{
    const auto redacted = ui::redactSensitiveText(
        QStringLiteral("password=hunter2; token: abc123; "
                       "Authorization=Bearer top.secret; "
                       "Cookie='session-value'"));
    QVERIFY(!redacted.contains(QStringLiteral("hunter2")));
    QVERIFY(!redacted.contains(QStringLiteral("abc123")));
    QVERIFY(!redacted.contains(QStringLiteral("top.secret")));
    QVERIFY(!redacted.contains(QStringLiteral("session-value")));
    QVERIFY(redacted.count(QStringLiteral("[redacted]")) >= 4);

    const auto json = ui::redactSensitiveText(QStringLiteral(
        R"({"api_key":"live-key","nested":{"authorization":"Bearer hidden"},"safe":"visible"})"));
    QVERIFY(!json.contains(QStringLiteral("live-key")));
    QVERIFY(!json.contains(QStringLiteral("hidden")));
    QVERIFY(json.contains(QStringLiteral("visible")));

    const auto truncated =
        ui::redactSensitiveText(QString(200, QLatin1Char('x')), 64);
    QCOMPARE(truncated.size(), 67);
    QVERIFY(truncated.endsWith(QStringLiteral("...")));
}

void AssistantResponseTest::validatesNewMcpServerConfiguration()
{
    QTemporaryDir packageDirectory;
    QVERIFY(packageDirectory.isValid());
#ifdef Q_OS_WIN
    const auto packageProgram =
        QDir(packageDirectory.path())
            .filePath(QStringLiteral("sample-server.exe"));
#else
    const auto packageProgram =
        QDir(packageDirectory.path()).filePath(QStringLiteral("sample-server"));
#endif
    QVERIFY(
        QFile::copy(QCoreApplication::applicationFilePath(), packageProgram));
#ifndef Q_OS_WIN
    QVERIFY(QFile::setPermissions(
        packageProgram, QFile::permissions(packageProgram) |
                            QFileDevice::ExeOwner | QFileDevice::ExeGroup |
                            QFileDevice::ExeOther));
#endif

    ui::McpServerDialog duplicateDialog({QStringLiteral("sample-server")});
    auto* duplicateFolderEdit = duplicateDialog.findChild<QLineEdit*>(
        QStringLiteral("fastMcpFolderEdit"));
    auto* duplicateErrorLabel =
        duplicateDialog.findChild<QLabel*>(QStringLiteral("errorLabel"));
    QVERIFY(duplicateFolderEdit != nullptr);
    QVERIFY(duplicateErrorLabel != nullptr);
    duplicateFolderEdit->setText(packageDirectory.path());
    QSignalSpy duplicateAcceptedSpy(&duplicateDialog, &QDialog::accepted);
    QVERIFY(QMetaObject::invokeMethod(&duplicateDialog, "accept"));
    QCOMPARE(duplicateAcceptedSpy.count(), 0);
    QVERIFY(!duplicateErrorLabel->isHidden());

    ui::McpServerDialog fastMcpDialog({QStringLiteral("existing")});
    auto* configurationTabs = fastMcpDialog.findChild<QTabWidget*>(
        QStringLiteral("configurationTabs"));
    auto* fastMcpFolderEdit = fastMcpDialog.findChild<QLineEdit*>(
        QStringLiteral("fastMcpFolderEdit"));
    QVERIFY(configurationTabs != nullptr);
    QVERIFY(fastMcpFolderEdit != nullptr);
    QCOMPARE(configurationTabs->currentIndex(), 0);
    fastMcpFolderEdit->setText(packageDirectory.path());
    QSignalSpy fastMcpAcceptedSpy(&fastMcpDialog, &QDialog::accepted);
    QVERIFY(QMetaObject::invokeMethod(&fastMcpDialog, "accept"));
    QCOMPARE(fastMcpAcceptedSpy.count(), 1);

    const auto fastMcpConfig = fastMcpDialog.configuration();
    QCOMPARE(fastMcpConfig.serverId, QStringLiteral("sample-server"));
    QCOMPARE(fastMcpConfig.program,
             QFileInfo(packageProgram).canonicalFilePath());
    QVERIFY(fastMcpConfig.arguments.isEmpty());
    QCOMPARE(fastMcpConfig.workingDirectory,
             QFileInfo(packageDirectory.path()).canonicalFilePath());
    QVERIFY(fastMcpConfig.enabled);

    ui::McpServerDialog customDialog({QStringLiteral("existing")});
    auto* customTabs = customDialog.findChild<QTabWidget*>(
        QStringLiteral("configurationTabs"));
    auto* serverIdEdit =
        customDialog.findChild<QLineEdit*>(QStringLiteral("serverIdEdit"));
    auto* programEdit =
        customDialog.findChild<QLineEdit*>(QStringLiteral("programEdit"));
    auto* argumentsEdit =
        customDialog.findChild<QLineEdit*>(QStringLiteral("argumentsEdit"));
    auto* workingDirectoryEdit = customDialog.findChild<QLineEdit*>(
        QStringLiteral("workingDirectoryEdit"));
    auto* authorizedRootsEdit = customDialog.findChild<QLineEdit*>(
        QStringLiteral("authorizedRootsEdit"));
    QVERIFY(customTabs != nullptr);
    QVERIFY(serverIdEdit != nullptr);
    QVERIFY(programEdit != nullptr);
    QVERIFY(argumentsEdit != nullptr);
    QVERIFY(workingDirectoryEdit != nullptr);
    QVERIFY(authorizedRootsEdit != nullptr);

    customTabs->setCurrentIndex(1);
    serverIdEdit->setText(QStringLiteral("custom-server"));
    programEdit->setText(packageProgram);
    argumentsEdit->setText(QStringLiteral("--stdio \"two words\""));
    workingDirectoryEdit->setText(packageDirectory.path());
    authorizedRootsEdit->setText(packageDirectory.path());
    QSignalSpy acceptedSpy(&customDialog, &QDialog::accepted);
    QVERIFY(QMetaObject::invokeMethod(&customDialog, "accept"));
    QCOMPARE(acceptedSpy.count(), 1);

    const auto config = customDialog.configuration();
    QCOMPARE(config.serverId, QStringLiteral("custom-server"));
    QCOMPARE(config.program, QFileInfo(packageProgram).absoluteFilePath());
    QCOMPARE(config.arguments, QStringList({QStringLiteral("--stdio"),
                                            QStringLiteral("two words")}));
    QCOMPARE(config.workingDirectory,
             QFileInfo(packageDirectory.path()).canonicalFilePath());
    QCOMPARE(
        config.authorizedRoots,
        QStringList{QFileInfo(packageDirectory.path()).canonicalFilePath()});
    QVERIFY(config.enabled);
}

void AssistantResponseTest::separatesModelLocationAndReloadActions()
{
    ui::ChatView view;
    view.setModelPresentation(QDir::homePath(), QStringLiteral("Test model"),
                              ui::ModelBadgeState::Verified);
    auto* modelLink = view.findChild<QLabel*>(QStringLiteral("modelPathLink"));
    auto* guideModelName =
        view.findChild<QLabel*>(QStringLiteral("guideModelNameLabel"));
    auto* reloadAction =
        view.findChild<QAction*>(QStringLiteral("modelReloadAction"));
    auto* modeSwitch =
        view.findChild<ui::ModeSwitch*>(QStringLiteral("agentModeSwitch"));
    auto* promptEditor =
        view.findChild<QPlainTextEdit*>(QStringLiteral("promptEditor"));
    auto* promptComposer =
        view.findChild<QWidget*>(QStringLiteral("promptComposer"));
    QVERIFY(modelLink != nullptr);
    QVERIFY(guideModelName != nullptr);
    QVERIFY(reloadAction != nullptr);
    QVERIFY(modeSwitch != nullptr);
    QVERIFY(promptEditor != nullptr);
    QVERIFY(promptComposer != nullptr);
    QVERIFY(!modeSwitch->isChecked());
    QCOMPARE(modeSwitch->text(), QStringLiteral("Agent"));
    QCOMPARE(promptEditor->placeholderText(),
             QStringLiteral("Write a message (Shift+Enter for a new line)"));
    QCOMPARE(promptComposer->property("agentMode").toBool(), false);
    QVERIFY(!modeSwitch->toolTip().isEmpty());
    QSignalSpy modeSpy(&view, &ui::ChatView::modeChanged);
    view.setAgentModeSelected(true);
    QVERIFY(modeSwitch->isChecked());
    QCOMPARE(promptEditor->placeholderText(),
             QStringLiteral(
                 "Describe a task for the agent (Shift+Enter for a new line)"));
    QCOMPARE(promptComposer->property("agentMode").toBool(), true);
    QCOMPARE(modeSpy.count(), 0);
    modeSwitch->click();
    QVERIFY(!modeSwitch->isChecked());
    QCOMPARE(modeSpy.count(), 1);
    QTest::mouseClick(modeSwitch, Qt::LeftButton);
    QVERIFY(modeSwitch->isChecked());
    QCOMPARE(modeSpy.count(), 2);
    QTest::mouseClick(modeSwitch, Qt::LeftButton);
    QVERIFY(!modeSwitch->isChecked());
    QCOMPARE(modeSpy.count(), 3);
    QVERIFY(view.findChild<QLabel*>(QStringLiteral("guideDescriptionLabel")) ==
            nullptr);
    QVERIFY(guideModelName->width() >= 520);
    QVERIFY(guideModelName->width() <= 600);
    QCOMPARE(modelLink->contextMenuPolicy(), Qt::ActionsContextMenu);
    QVERIFY(modelLink->actions().contains(reloadAction));
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

    reloadAction->trigger();
    QCOMPARE(locationRequested.count(), 1);
    QCOMPARE(folderRequested.count(), 1);
}

void AssistantResponseTest::presentsComputeSettingsOnStartPage()
{
    ui::ChatView view;
    auto* summary =
        view.findChild<QLabel*>(QStringLiteral("guideComputeSummaryLabel"));
    auto* button = view.findChild<QToolButton*>(
        QStringLiteral("guideComputeSettingsButton"));
    auto* action =
        view.findChild<QAction*>(QStringLiteral("computeSettingsAction"));
    QVERIFY(summary != nullptr);
    QVERIFY(button != nullptr);
    QVERIFY(action != nullptr);
    QVERIFY(!button->icon().isNull());
    QVERIFY(!button->toolTip().isEmpty());
    QVERIFY(!button->accessibleName().isEmpty());

    view.setComputePresentation(QStringLiteral("Compute: Custom - 2 GPUs"));
    QCOMPARE(summary->text(), QStringLiteral("Compute: Custom - 2 GPUs"));

    view.setComputeSettingsEnabled(false);
    QVERIFY(!button->isEnabled());
    QVERIFY(!action->isEnabled());
    view.setComputeSettingsEnabled(true);

    QSignalSpy settingsRequested(&view,
                                 &ui::ChatView::computeSettingsRequested);
    button->click();
    QCOMPARE(settingsRequested.count(), 1);
}

void AssistantResponseTest::
    placesModelControlsInComposerAndMergesPrimaryAction()
{
    ui::MainWindow window(settingsFilePath());
    auto* chatView = qobject_cast<ui::ChatView*>(window.centralWidget());
    auto* composer =
        window.findChild<QWidget*>(QStringLiteral("promptComposer"));
    auto* modelLink =
        window.findChild<QLabel*>(QStringLiteral("modelPathLink"));
    auto* modelReloadAction =
        window.findChild<QAction*>(QStringLiteral("modelReloadAction"));
    auto* modeSwitch =
        window.findChild<ui::ModeSwitch*>(QStringLiteral("agentModeSwitch"));
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
    auto* rootLayout =
        window.findChild<QVBoxLayout*>(QStringLiteral("rootLayout"));

    QVERIFY(chatView != nullptr);
    QCOMPARE(window.centralWidget(), chatView);
    QVERIFY(composer != nullptr);
    QVERIFY(modelLink != nullptr);
    QVERIFY(modelReloadAction != nullptr);
    QVERIFY(modeSwitch != nullptr);
    QVERIFY(guideLoadButton != nullptr);
    QVERIFY(guideStatus != nullptr);
    QVERIFY(statusLabel != nullptr);
    QVERIFY(primaryAction != nullptr);
    QVERIFY(contentStack != nullptr);
    QVERIFY(transcriptPage != nullptr);
    QVERIFY(transcriptTabs != nullptr);
    QVERIFY(rootLayout != nullptr);
    QCOMPARE(modelLink->parentWidget(), composer);
    QCOMPARE(modeSwitch->parentWidget(), composer);
    QVERIFY(modelLink->actions().contains(modelReloadAction));
    QVERIFY(!transcriptTabs->actions().contains(modelReloadAction));
    QCOMPARE(transcriptTabs->tabPosition(), QTabWidget::South);
    QCOMPARE(transcriptTabs->contextMenuPolicy(), Qt::ActionsContextMenu);
    QCOMPARE(primaryAction->parentWidget(), composer);
    QCOMPARE(rootLayout->stretch(0), 1);
    QCOMPARE(rootLayout->stretch(1), 0);
    QCOMPARE(composer->sizePolicy().verticalPolicy(), QSizePolicy::Maximum);
    QCOMPARE(composer->maximumHeight(), 150);
    QVERIFY(!primaryAction->icon().isNull());
    QVERIFY(!modelLink->text().contains(QStringLiteral("<a ")));
    QVERIFY(!modelLink->isEnabled());
    QVERIFY(!modelLink->toolTip().isEmpty());
    QVERIFY(!modelReloadAction->icon().isNull());
    QCOMPARE(modelReloadAction->text(),
             QStringLiteral("Change or reload model"));
    QVERIFY(!modelReloadAction->toolTip().isEmpty());
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
    QVERIFY(window.findChild<QToolButton*>(
                QStringLiteral("modelReloadButton")) == nullptr);
    QVERIFY(window.findChild<QToolButton*>(
                QStringLiteral("clearConversationButton")) == nullptr);
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
