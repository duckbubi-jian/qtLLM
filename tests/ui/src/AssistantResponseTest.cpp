#include "AssistantResponse.hpp"
#include "MainWindow.hpp"
#include "MessageWidget.hpp"
#include "Theme.hpp"
#include "ToolApprovalWidget.hpp"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextFormat>
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
    void enterSendsAndShiftEnterAddsNewline();
    void restoresPromptAfterGenerationError();
    void clearsVisibleAndInMemoryConversation();
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
    QVERIFY(arguments != nullptr);
    QVERIFY(allow != nullptr);
    QVERIFY(
        arguments->toPlainText().contains(QStringLiteral("D:/safe/file.txt")));
    QVERIFY(arguments->toPlainText().contains(QStringLiteral("[redacted]")));
    QVERIFY(
        !arguments->toPlainText().contains(QStringLiteral("do-not-display")));
    QVERIFY(!arguments->toPlainText().contains(QStringLiteral("also-hidden")));

    allow->click();
    QCOMPARE(decisionSpy.count(), 1);
    QVERIFY(decisionSpy.constFirst().constFirst().toBool());
    QVERIFY(!allow->isVisible());
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
    auto* rawTranscript =
        window.findChild<QPlainTextEdit*>(QStringLiteral("rawTranscript"));
    auto* clearButton = window.findChild<QPushButton*>(
        QStringLiteral("clearConversationButton"));
    QVERIFY(prompt != nullptr);
    QVERIFY(rawTranscript != nullptr);
    QVERIFY(clearButton != nullptr);
    QVERIFY(!clearButton->isEnabled());

    prompt->setPlainText(QStringLiteral("temporary message"));
    QVERIFY(QMetaObject::invokeMethod(&window, "sendPrompt"));
    QCOMPARE(window.findChildren<ui::MessageWidget*>().size(), 1);
    QVERIFY(clearButton->isEnabled());

    clearButton->click();
    QCOMPARE(window.findChildren<ui::MessageWidget*>().size(), 0);
    QVERIFY(!clearButton->isEnabled());
    QVERIFY(rawTranscript->toPlainText().isEmpty());
}
}  // namespace qtllm::tests

QTEST_MAIN(qtllm::tests::AssistantResponseTest)

#include "AssistantResponseTest.moc"
