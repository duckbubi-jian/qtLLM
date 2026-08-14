#include "ToolApprovalWidget.hpp"

#include "SensitiveData.hpp"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace qtllm::ui
{
namespace
{
struct RiskPresentation
{
    QString level;
    QString title;
    QString description;
    QStyle::StandardPixmap icon;
};

RiskPresentation presentationFor(infrastructure::mcp::ToolRisk risk)
{
    using infrastructure::mcp::ToolRisk;
    switch (risk)
    {
        case ToolRisk::ReadOnly:
            return {QStringLiteral("read"),
                    ToolApprovalWidget::tr("Data access request"),
                    ToolApprovalWidget::tr(
                        "This tool can read data from its configured scope."),
                    QStyle::SP_MessageBoxInformation};
        case ToolRisk::CreatesData:
            return {QStringLiteral("create"),
                    ToolApprovalWidget::tr("Data creation request"),
                    ToolApprovalWidget::tr(
                        "This tool can create files, records, or other data."),
                    QStyle::SP_MessageBoxWarning};
        case ToolRisk::Destructive:
            return {QStringLiteral("destructive"),
                    ToolApprovalWidget::tr("Destructive operation request"),
                    ToolApprovalWidget::tr(
                        "This tool may delete or irreversibly overwrite data."),
                    QStyle::SP_MessageBoxCritical};
        case ToolRisk::ModifiesData:
        default:
            return {QStringLiteral("modify"),
                    ToolApprovalWidget::tr("Data modification request"),
                    ToolApprovalWidget::tr(
                        "This unclassified tool may change or delete files, "
                        "records, or external state."),
                    QStyle::SP_MessageBoxWarning};
    }
}

}  // namespace

ToolApprovalWidget::ToolApprovalWidget(infrastructure::mcp::ToolRisk risk,
                                       const QString& toolName,
                                       const QJsonObject& arguments,
                                       QWidget* parent)
    : QWidget(parent)
{
    const auto presentation = presentationFor(risk);
    setObjectName(QStringLiteral("toolApprovalCard"));
    setProperty("riskLevel", presentation.level);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto* headingRow = new QHBoxLayout;
    headingRow->setContentsMargins(0, 0, 0, 0);
    headingRow->setSpacing(8);
    auto* iconLabel = new QLabel(this);
    iconLabel->setPixmap(
        style()->standardIcon(presentation.icon).pixmap(20, 20));
    headingRow->addWidget(iconLabel, 0, Qt::AlignTop);
    auto* heading = new QLabel(presentation.title, this);
    heading->setObjectName(QStringLiteral("toolApprovalHeading"));
    headingRow->addWidget(heading, 1);
    layout->addLayout(headingRow);

    auto* description = new QLabel(presentation.description, this);
    description->setObjectName(QStringLiteral("toolApprovalDescription"));
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* toolLabel = new QLabel(tr("Tool: %1").arg(toolName), this);
    toolLabel->setObjectName(QStringLiteral("toolApprovalName"));
    toolLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(toolLabel);

    auto* argumentLabel = new QLabel(tr("Arguments"), this);
    argumentLabel->setObjectName(QStringLiteral("toolApprovalArgumentLabel"));
    layout->addWidget(argumentLabel);

    auto* argumentView = new QPlainTextEdit(this);
    argumentView->setObjectName(QStringLiteral("toolApprovalArguments"));
    argumentView->setReadOnly(true);
    argumentView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    argumentView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    argumentView->setPlainText(QString::fromUtf8(
        QJsonDocument(redactSensitiveValues(arguments).toObject())
            .toJson(QJsonDocument::Indented)));
    const auto visibleLines =
        qBound(3, argumentView->document()->blockCount(), 8);
    argumentView->setFixedHeight(
        visibleLines * argumentView->fontMetrics().lineSpacing() + 18);
    layout->addWidget(argumentView);

    auto* actionRow = new QHBoxLayout;
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(8);
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("toolApprovalStatus"));
    statusLabel_->setVisible(false);
    actionRow->addWidget(statusLabel_);
    actionRow->addStretch();
    rejectButton_ = new QPushButton(tr("Reject"), this);
    rejectButton_->setObjectName(QStringLiteral("rejectToolButton"));
    allowButton_ = new QPushButton(tr("Allow once"), this);
    allowButton_->setObjectName(QStringLiteral("allowToolButton"));
    actionRow->addWidget(rejectButton_);
    actionRow->addWidget(allowButton_);
    layout->addLayout(actionRow);

    connect(allowButton_, &QPushButton::clicked, this,
            [this] { resolve(true); });
    connect(rejectButton_, &QPushButton::clicked, this,
            [this] { resolve(false); });
}

void ToolApprovalWidget::markCancelled()
{
    if (!resolved_) setResolvedState(tr("No longer active"), false);
}

void ToolApprovalWidget::resolve(bool approved)
{
    if (resolved_) return;
    setResolvedState(approved ? tr("Allowed once") : tr("Rejected"), approved);
    emit decisionMade(approved);
}

void ToolApprovalWidget::setResolvedState(const QString& status, bool approved)
{
    resolved_ = true;
    allowButton_->setEnabled(false);
    rejectButton_->setEnabled(false);
    allowButton_->setVisible(false);
    rejectButton_->setVisible(false);
    statusLabel_->setText(status);
    statusLabel_->setProperty("approved", approved);
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
    statusLabel_->setVisible(true);
}
}  // namespace qtllm::ui
