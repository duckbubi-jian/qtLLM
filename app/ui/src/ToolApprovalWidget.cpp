#include "ToolApprovalWidget.hpp"

#include "SensitiveData.hpp"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
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
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(5);

    auto* headingRow = new QHBoxLayout;
    headingRow->setContentsMargins(0, 0, 0, 0);
    headingRow->setSpacing(8);
    auto* iconLabel = new QLabel(this);
    iconLabel->setPixmap(
        style()->standardIcon(presentation.icon).pixmap(20, 20));
    headingRow->addWidget(iconLabel);
    detailsToggle_ = new QToolButton(this);
    detailsToggle_->setObjectName(QStringLiteral("toolApprovalToggle"));
    detailsToggle_->setCheckable(true);
    detailsToggle_->setChecked(true);
    detailsToggle_->setArrowType(Qt::DownArrow);
    detailsToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    detailsToggle_->setText(presentation.title);
    detailsToggle_->setAutoRaise(true);
    detailsToggle_->setAccessibleName(tr("Tool approval details"));
    headingRow->addWidget(detailsToggle_);
    headingRow->addStretch(1);
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("toolApprovalStatus"));
    statusLabel_->setVisible(false);
    headingRow->addWidget(statusLabel_);
    layout->addLayout(headingRow);

    details_ = new QWidget(this);
    details_->setObjectName(QStringLiteral("toolApprovalDetails"));
    auto* detailsLayout = new QVBoxLayout(details_);
    detailsLayout->setContentsMargins(28, 0, 0, 0);
    detailsLayout->setSpacing(5);
    layout->addWidget(details_);

    descriptionLabel_ = new QLabel(presentation.description, details_);
    descriptionLabel_->setObjectName(QStringLiteral("toolApprovalDescription"));
    descriptionLabel_->setWordWrap(true);
    detailsLayout->addWidget(descriptionLabel_);

    auto* toolLabel = new QLabel(tr("Tool: %1").arg(toolName), details_);
    toolLabel->setObjectName(QStringLiteral("toolApprovalName"));
    toolLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailsLayout->addWidget(toolLabel);

    argumentToggle_ = new QToolButton(details_);
    argumentToggle_->setObjectName(
        QStringLiteral("toolApprovalArgumentsToggle"));
    argumentToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    argumentToggle_->setArrowType(Qt::RightArrow);
    argumentToggle_->setCheckable(true);
    argumentToggle_->setText(tr("Arguments"));
    argumentToggle_->setAutoRaise(true);
    argumentToggle_->setAccessibleName(tr("Tool arguments"));
    detailsLayout->addWidget(argumentToggle_, 0, Qt::AlignLeft);

    argumentView_ = new QPlainTextEdit(details_);
    argumentView_->setObjectName(QStringLiteral("toolApprovalArguments"));
    argumentView_->setReadOnly(true);
    argumentView_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    argumentView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    argumentView_->setPlainText(QString::fromUtf8(
        QJsonDocument(redactSensitiveValues(arguments).toObject())
            .toJson(QJsonDocument::Indented)));
    const auto visibleLines =
        qBound(3, argumentView_->document()->blockCount(), 6);
    argumentView_->setFixedHeight(
        visibleLines * argumentView_->fontMetrics().lineSpacing() + 14);
    argumentView_->setVisible(false);
    detailsLayout->addWidget(argumentView_);

    auto* actionRow = new QHBoxLayout;
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(8);
    actionRow->addStretch();
    rejectButton_ = new QPushButton(tr("Reject"), details_);
    rejectButton_->setObjectName(QStringLiteral("rejectToolButton"));
    allowButton_ = new QPushButton(tr("Allow once"), details_);
    allowButton_->setObjectName(QStringLiteral("allowToolButton"));
    alwaysAllowButton_ = new QPushButton(tr("Always allow"), details_);
    alwaysAllowButton_->setObjectName(QStringLiteral("alwaysAllowToolButton"));
    alwaysAllowButton_->setVisible(risk !=
                                   infrastructure::mcp::ToolRisk::Destructive);
    actionRow->addWidget(rejectButton_);
    actionRow->addWidget(allowButton_);
    actionRow->addWidget(alwaysAllowButton_);
    detailsLayout->addLayout(actionRow);

    connect(allowButton_, &QPushButton::clicked, this,
            [this] { resolve(ToolApprovalDecision::AllowOnce); });
    connect(alwaysAllowButton_, &QPushButton::clicked, this,
            [this] { resolve(ToolApprovalDecision::AlwaysAllow); });
    connect(rejectButton_, &QPushButton::clicked, this,
            [this] { resolve(ToolApprovalDecision::DenyOnce); });
    connect(detailsToggle_, &QToolButton::toggled, this,
            [this](bool expanded)
            {
                detailsToggle_->setArrowType(expanded ? Qt::DownArrow
                                                      : Qt::RightArrow);
                details_->setVisible(expanded);
            });
    connect(argumentToggle_, &QToolButton::toggled, this,
            [this](bool expanded)
            {
                argumentToggle_->setArrowType(expanded ? Qt::DownArrow
                                                       : Qt::RightArrow);
                argumentView_->setVisible(expanded);
            });
}

void ToolApprovalWidget::markCancelled()
{
    if (!resolved_) setResolvedState(tr("No longer active"), false);
}

void ToolApprovalWidget::resolve(ToolApprovalDecision decision)
{
    if (resolved_) return;
    const auto approved = decision != ToolApprovalDecision::DenyOnce;
    switch (decision)
    {
        case ToolApprovalDecision::DenyOnce:
            setResolvedState(tr("Denied once"), false);
            break;
        case ToolApprovalDecision::AllowOnce:
            setResolvedState(tr("Allowed once"), true);
            break;
        case ToolApprovalDecision::AlwaysAllow:
            setResolvedState(tr("Always allowed"), true);
            break;
    }
    emit decisionMade(decision);
}

void ToolApprovalWidget::setResolvedState(const QString& status, bool approved)
{
    resolved_ = true;
    allowButton_->setEnabled(false);
    alwaysAllowButton_->setEnabled(false);
    rejectButton_->setEnabled(false);
    allowButton_->setVisible(false);
    alwaysAllowButton_->setVisible(false);
    rejectButton_->setVisible(false);
    descriptionLabel_->setVisible(false);
    argumentToggle_->setVisible(false);
    argumentView_->setVisible(false);
    detailsToggle_->setChecked(false);
    statusLabel_->setText(status);
    statusLabel_->setProperty("approved", approved);
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
    statusLabel_->setVisible(true);
}
}  // namespace qtllm::ui
