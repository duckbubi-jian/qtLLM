#include "MessageWidget.hpp"

#include "AssistantResponse.hpp"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace qtllm::ui
{
namespace
{
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
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(5);

    roleLabel_ =
        new QLabel(role == Role::User ? tr("You") : tr("Assistant"), this);
    auto roleFont = roleLabel_->font();
    roleFont.setBold(true);
    roleLabel_->setFont(roleFont);
    layout->addWidget(roleLabel_);

    reasoningToggle_ = new QToolButton(this);
    reasoningToggle_->setCheckable(true);
    reasoningToggle_->setChecked(false);
    reasoningToggle_->setArrowType(Qt::RightArrow);
    reasoningToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    reasoningToggle_->setText(tr("Reasoning"));
    reasoningToggle_->setVisible(false);
    layout->addWidget(reasoningToggle_, 0, Qt::AlignLeft);

    reasoningView_ = new AutoSizingTextBrowser(this);
    reasoningView_->setVisible(false);
    reasoningView_->document()->setDocumentMargin(8.0);
    reasoningView_->setStyleSheet(
        QStringLiteral("QTextBrowser { color: palette(mid); "
                       "background: palette(alternate-base); "
                       "border-left: 3px solid palette(midlight); }"));
    layout->addWidget(reasoningView_);

    bodyView_ = new AutoSizingTextBrowser(this);
    if (role == Role::User)
    {
        bodyView_->document()->setDocumentMargin(8.0);
        bodyView_->setStyleSheet(QStringLiteral(
            "QTextBrowser { background: palette(alternate-base); "
            "border: 1px solid palette(midlight); "
            "border-radius: 6px; }"));
    }
    layout->addWidget(bodyView_);

    codeActions_ = new QWidget(this);
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
}

void MessageWidget::setAssistantText(const QString& rawText, bool final)
{
    if (role_ != Role::Assistant) return;

    const auto response = parseAssistantResponse(rawText);
    const auto reasoningOnly =
        final && response.answer.isEmpty() && !response.reasoning.isEmpty();
    if (response.hasReasoning && !reasoningOnly)
    {
        reasoningToggle_->setVisible(true);
        reasoningToggle_->setText(response.reasoningComplete || final
                                      ? tr("Reasoning")
                                      : tr("Reasoning..."));
        reasoningView_->setMarkdown(response.reasoning);
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

    if (!final) return;

    clearLayout(codeActionsLayout_);
    const auto codeBlocks = fencedCodeBlocks(visibleAnswer);
    for (qsizetype index = 0; index < codeBlocks.size(); ++index)
    {
        const auto& block = codeBlocks.at(index);
        auto* row = new QWidget(codeActions_);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto* label = new QLabel(
            block.language.isEmpty()
                ? tr("Code %1").arg(index + 1)
                : tr("Code %1 - %2").arg(index + 1).arg(block.language),
            row);
        auto* copyButton = new QToolButton(row);
        copyButton->setText(tr("Copy code"));
        copyButton->setToolTip(tr("Copy this code block"));
        rowLayout->addWidget(label);
        rowLayout->addStretch();
        rowLayout->addWidget(copyButton);
        codeActionsLayout_->addWidget(row);
        connect(copyButton, &QToolButton::clicked, this, [code = block.code]
                { QApplication::clipboard()->setText(code); });
    }
    codeActions_->setVisible(!codeBlocks.isEmpty());
}
}  // namespace qtllm::ui
