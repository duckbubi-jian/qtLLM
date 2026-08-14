#include "MessageWidget.hpp"

#include "AssistantResponse.hpp"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace qtllm::ui
{
namespace
{
const QString& markdownStyleSheet()
{
    static const QString styleSheet = QStringLiteral(R"(
body { color: #202124; }
p { margin-top: 0; margin-bottom: 10px; }
h1 { font-size: 18pt; font-weight: 600; margin-top: 14px; margin-bottom: 8px; }
h2 { font-size: 15pt; font-weight: 600; margin-top: 12px; margin-bottom: 7px; }
h3 { font-size: 12pt; font-weight: 600; margin-top: 10px; margin-bottom: 6px; }
ul, ol { margin-top: 4px; margin-bottom: 10px; }
li { margin-bottom: 4px; }
blockquote {
    color: #5f6368;
    background-color: #f6f7f9;
    border-left: 3px solid #9aa0a6;
    margin: 8px 0;
    padding: 6px 10px;
}
pre {
    color: #e6edf3;
    background-color: #1f2937;
    border: 1px solid #111827;
    margin: 8px 0 10px 0;
    padding: 10px;
    white-space: pre-wrap;
}
code {
    color: #b42318;
    background-color: #f1f3f5;
    font-family: "Cascadia Mono", "Consolas", monospace;
}
table { border-collapse: collapse; margin: 8px 0 10px 0; }
th { background-color: #f1f3f5; font-weight: 600; }
th, td { border: 1px solid #d7dbe0; padding: 5px 8px; }
a { color: #1d4ed8; text-decoration: none; }
hr { color: #dfe3e8; }
)");
    return styleSheet;
}

bool isMarkdownCodeBlock(const QTextBlock& block)
{
    if (!block.isValid()) return false;
    const auto format = block.blockFormat();
    return format.hasProperty(QTextFormat::BlockCodeFence) ||
           format.hasProperty(QTextFormat::BlockCodeLanguage);
}

void polishMarkdownDocument(QTextDocument& document)
{
    for (auto block = document.begin(); block.isValid(); block = block.next())
    {
        auto blockFormat = block.blockFormat();
        const auto isCodeBlock = isMarkdownCodeBlock(block);
        const auto quoteLevel =
            blockFormat.property(QTextFormat::BlockQuoteLevel).toInt();

        QTextCursor cursor(block);
        if (isCodeBlock)
        {
            blockFormat.setBackground(QColor(QStringLiteral("#1f2937")));
            blockFormat.setLeftMargin(12.0);
            blockFormat.setRightMargin(12.0);
            blockFormat.setTopMargin(
                isMarkdownCodeBlock(block.previous()) ? 0.0 : 6.0);
            blockFormat.setBottomMargin(
                isMarkdownCodeBlock(block.next()) ? 0.0 : 6.0);
            cursor.setBlockFormat(blockFormat);

            cursor.select(QTextCursor::BlockUnderCursor);
            QTextCharFormat codeFormat;
            codeFormat.setForeground(QColor(QStringLiteral("#e6edf3")));
            codeFormat.setFont(
                QFontDatabase::systemFont(QFontDatabase::FixedFont));
            cursor.mergeCharFormat(codeFormat);
        }
        else if (quoteLevel > 0)
        {
            blockFormat.setBackground(QColor(QStringLiteral("#f1f3f5")));
            blockFormat.setLeftMargin(14.0 + quoteLevel * 8.0);
            blockFormat.setRightMargin(8.0);
            blockFormat.setTopMargin(5.0);
            blockFormat.setBottomMargin(5.0);
            cursor.setBlockFormat(blockFormat);

            cursor.select(QTextCursor::BlockUnderCursor);
            QTextCharFormat quoteFormat;
            quoteFormat.setForeground(QColor(QStringLiteral("#5f6368")));
            cursor.mergeCharFormat(quoteFormat);
        }
    }
}

struct CodeBlock
{
    QString language;
    QString code;
};

QList<CodeBlock> fencedCodeBlocks(const QString& markdown)
{
    QList<CodeBlock> blocks;
    qsizetype position = 0;
    while (true)
    {
        const auto opening = markdown.indexOf(QStringLiteral("```"), position);
        if (opening < 0) break;
        const auto firstNewline = markdown.indexOf(QLatin1Char('\n'), opening);
        if (firstNewline < 0) break;
        const auto closing =
            markdown.indexOf(QStringLiteral("```"), firstNewline + 1);
        if (closing < 0) break;

        blocks.append(
            {markdown.mid(opening + 3, firstNewline - opening - 3).trimmed(),
             markdown.mid(firstNewline + 1, closing - firstNewline - 1)});
        position = closing + 3;
    }
    return blocks;
}

void clearLayout(QLayout* layout)
{
    while (auto* item = layout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
}

class AutoSizingTextBrowser final : public QTextBrowser
{
   public:
    explicit AutoSizingTextBrowser(QWidget* parent = nullptr)
        : QTextBrowser(parent)
    {
        setReadOnly(true);
        setOpenExternalLinks(false);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        document()->setDocumentMargin(0.0);
        connect(document()->documentLayout(),
                &QAbstractTextDocumentLayout::documentSizeChanged, this,
                [this] { scheduleHeightUpdate(); });
    }

   protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QTextBrowser::resizeEvent(event);
        document()->setTextWidth(viewport()->width());
        scheduleHeightUpdate();
    }

   private:
    void scheduleHeightUpdate()
    {
        QTimer::singleShot(0, this,
                           [this]
                           {
                               const auto documentHeight = static_cast<int>(
                                   std::ceil(document()->size().height()));
                               setFixedHeight(qMax(fontMetrics().height(),
                                                   documentHeight + 2));
                           });
    }
};
}  // namespace

MessageWidget::MessageWidget(Role role, QWidget* parent)
    : QWidget(parent), role_(role)
{
    setObjectName(role == Role::User ? QStringLiteral("userMessage")
                                     : QStringLiteral("assistantMessage"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(6);

    roleLabel_ =
        new QLabel(role == Role::User ? tr("You") : tr("Assistant"), this);
    roleLabel_->setObjectName(role == Role::User
                                  ? QStringLiteral("userRoleLabel")
                                  : QStringLiteral("assistantRoleLabel"));
    auto* roleRow = new QHBoxLayout;
    roleRow->setContentsMargins(0, 0, 0, 0);
    if (role == Role::User) roleRow->addStretch();
    roleRow->addWidget(roleLabel_);
    if (role == Role::Assistant) roleRow->addStretch();
    layout->addLayout(roleRow);

    reasoningToggle_ = new QToolButton(this);
    reasoningToggle_->setObjectName(QStringLiteral("reasoningToggle"));
    reasoningToggle_->setCheckable(true);
    reasoningToggle_->setChecked(false);
    reasoningToggle_->setArrowType(Qt::RightArrow);
    reasoningToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    reasoningToggle_->setText(tr("Reasoning"));
    reasoningToggle_->setVisible(false);
    layout->addWidget(reasoningToggle_, 0, Qt::AlignLeft);

    reasoningView_ = new AutoSizingTextBrowser(this);
    reasoningView_->setObjectName(QStringLiteral("reasoningBody"));
    reasoningView_->setVisible(false);
    reasoningView_->document()->setDocumentMargin(10.0);
    reasoningView_->document()->setDefaultStyleSheet(markdownStyleSheet());
    layout->addWidget(reasoningView_);

    bodyView_ = new AutoSizingTextBrowser(this);
    bodyView_->document()->setDefaultStyleSheet(markdownStyleSheet());
    if (role == Role::User)
    {
        bodyView_->setObjectName(QStringLiteral("userMessageBody"));
        bodyView_->document()->setDocumentMargin(11.0);
        auto* bodyRow = new QHBoxLayout;
        bodyRow->setContentsMargins(0, 0, 0, 0);
        bodyRow->addStretch();
        bodyRow->addWidget(bodyView_, 0, Qt::AlignTop | Qt::AlignRight);
        layout->addLayout(bodyRow);
    }
    else
    {
        bodyView_->setObjectName(QStringLiteral("assistantMessageBody"));
        bodyView_->document()->setDocumentMargin(0.0);
        layout->addWidget(bodyView_);
    }

    codeActions_ = new QWidget(this);
    codeActions_->setObjectName(QStringLiteral("codeActions"));
    codeActionsLayout_ = new QVBoxLayout(codeActions_);
    codeActionsLayout_->setContentsMargins(0, 0, 0, 0);
    codeActionsLayout_->setSpacing(4);
    codeActions_->setVisible(false);
    layout->addWidget(codeActions_);

    connect(reasoningToggle_, &QToolButton::toggled, this,
            [this](bool expanded)
            {
                reasoningToggle_->setArrowType(expanded ? Qt::DownArrow
                                                        : Qt::RightArrow);
                reasoningView_->setVisible(expanded);
            });
}

void MessageWidget::setUserText(const QString& text)
{
    if (role_ != Role::User) return;
    bodyView_->setPlainText(text);
    QTimer::singleShot(0, this, [this] { updateUserBubbleWidth(); });
}

void MessageWidget::setAssistantText(const QString& rawText, bool final)
{
    if (role_ != Role::Assistant) return;

    const auto response = chat::parseAssistantResponse(rawText);
    const auto reasoningOnly =
        final && response.answer.isEmpty() && !response.reasoning.isEmpty();
    if (response.hasReasoning && !reasoningOnly)
    {
        reasoningToggle_->setVisible(true);
        reasoningToggle_->setText(response.reasoningComplete || final
                                      ? tr("Reasoning")
                                      : tr("Reasoning..."));
        reasoningView_->setMarkdown(response.reasoning);
        polishMarkdownDocument(*reasoningView_->document());
        if (!hadReasoning_)
        {
            reasoningToggle_->setChecked(false);
            hadReasoning_ = true;
        }
    }
    else
    {
        reasoningToggle_->setVisible(false);
        reasoningView_->setVisible(false);
    }

    const auto visibleAnswer =
        reasoningOnly ? response.reasoning : response.answer;
    bodyView_->setVisible(!visibleAnswer.isEmpty() || final);
    bodyView_->setMarkdown(visibleAnswer);
    polishMarkdownDocument(*bodyView_->document());

    if (!final) return;

    clearLayout(codeActionsLayout_);
    const auto codeBlocks = fencedCodeBlocks(visibleAnswer);
    for (qsizetype index = 0; index < codeBlocks.size(); ++index)
    {
        const auto& block = codeBlocks.at(index);
        auto* row = new QWidget(codeActions_);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        auto* label = new QLabel(
            block.language.isEmpty()
                ? tr("Code %1").arg(index + 1)
                : tr("Code %1 - %2").arg(index + 1).arg(block.language),
            row);
        auto* copyButton = new QToolButton(row);
        copyButton->setObjectName(QStringLiteral("messageToolButton"));
        copyButton->setText(tr("Copy code"));
        copyButton->setToolTip(tr("Copy this code block"));
        rowLayout->addWidget(label);
        rowLayout->addWidget(copyButton);
        codeActionsLayout_->addWidget(row, 0, Qt::AlignLeft);
        connect(copyButton, &QToolButton::clicked, this, [code = block.code]
                { QApplication::clipboard()->setText(code); });
    }
    codeActions_->setVisible(!codeBlocks.isEmpty());
}

void MessageWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateUserBubbleWidth();
}

void MessageWidget::updateUserBubbleWidth()
{
    if (role_ != Role::User || bodyView_ == nullptr || width() <= 0) return;

    const auto maximumBubbleWidth = qMax(80, qMin(760, width() * 72 / 100));
    auto naturalWidth = 0;
    const auto lines = bodyView_->toPlainText().split(QLatin1Char('\n'));
    for (const auto& line : lines)
        naturalWidth = qMax(naturalWidth,
                            bodyView_->fontMetrics().horizontalAdvance(line));
    naturalWidth += 28;
    const auto minimumBubbleWidth = qMin(120, maximumBubbleWidth);
    const auto bubbleWidth =
        qBound(minimumBubbleWidth, naturalWidth, maximumBubbleWidth);
    if (bodyView_->width() != bubbleWidth)
        bodyView_->setFixedWidth(bubbleWidth);
}
}  // namespace qtllm::ui
