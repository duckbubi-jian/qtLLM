#include "VerificationConfirmationWidget.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace qtllm::ui
{
VerificationConfirmationWidget::VerificationConfirmationWidget(
    const QString& reason, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("verificationConfirmationCard"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(7);

    auto* headingRow = new QHBoxLayout;
    headingRow->setContentsMargins(0, 0, 0, 0);
    headingRow->setSpacing(8);
    auto* iconLabel = new QLabel(this);
    iconLabel->setPixmap(
        style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(20, 20));
    headingRow->addWidget(iconLabel);
    auto* title = new QLabel(tr("Verification needs your input"), this);
    title->setObjectName(QStringLiteral("verificationConfirmationTitle"));
    headingRow->addWidget(title);
    headingRow->addStretch(1);
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(
        QStringLiteral("verificationConfirmationStatus"));
    statusLabel_->setVisible(false);
    headingRow->addWidget(statusLabel_);
    layout->addLayout(headingRow);

    reasonLabel_ = new QLabel(reason, this);
    reasonLabel_->setObjectName(QStringLiteral("verificationReason"));
    reasonLabel_->setWordWrap(true);
    reasonLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(reasonLabel_);

    evidenceLabel_ = new QLabel(tr("Temporary evidence"), this);
    evidenceLabel_->setObjectName(QStringLiteral("temporaryEvidenceLabel"));
    layout->addWidget(evidenceLabel_);

    evidenceInput_ = new QPlainTextEdit(this);
    evidenceInput_->setObjectName(QStringLiteral("temporaryEvidenceInput"));
    evidenceInput_->setPlaceholderText(
        tr("Add what you checked or other evidence for this task"));
    evidenceInput_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    evidenceInput_->setFixedHeight(
        evidenceInput_->fontMetrics().lineSpacing() * 4 + 18);
    layout->addWidget(evidenceInput_);

    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    actions->addStretch(1);
    stopButton_ = new QPushButton(tr("Stop run"), this);
    stopButton_->setObjectName(QStringLiteral("stopUnverifiedRunButton"));
    submitEvidenceButton_ = new QPushButton(tr("Submit evidence"), this);
    submitEvidenceButton_->setObjectName(
        QStringLiteral("submitTemporaryEvidenceButton"));
    submitEvidenceButton_->setEnabled(false);
    acceptButton_ = new QPushButton(tr("Accept current result"), this);
    acceptButton_->setObjectName(
        QStringLiteral("acceptUnverifiedResultButton"));
    actions->addWidget(stopButton_);
    actions->addWidget(submitEvidenceButton_);
    actions->addWidget(acceptButton_);
    layout->addLayout(actions);

    connect(evidenceInput_, &QPlainTextEdit::textChanged, this,
            [this]
            {
                submitEvidenceButton_->setEnabled(
                    !evidenceInput_->toPlainText().trimmed().isEmpty());
            });
    connect(stopButton_, &QPushButton::clicked, this,
            [this] { resolve(application::VerificationDecision::Stop); });
    connect(submitEvidenceButton_, &QPushButton::clicked, this, [this]
            { resolve(application::VerificationDecision::ProvideEvidence); });
    connect(acceptButton_, &QPushButton::clicked, this, [this]
            { resolve(application::VerificationDecision::AcceptUnverified); });
}

void VerificationConfirmationWidget::markCancelled()
{
    if (!resolved_) setResolvedState(tr("No longer active"), false);
}

void VerificationConfirmationWidget::resolve(
    application::VerificationDecision decision)
{
    if (resolved_) return;
    const auto evidence = evidenceInput_->toPlainText().trimmed();
    switch (decision)
    {
        case application::VerificationDecision::AcceptUnverified:
            setResolvedState(tr("Result accepted"), true);
            break;
        case application::VerificationDecision::ProvideEvidence:
            if (evidence.isEmpty()) return;
            setResolvedState(tr("Evidence submitted"), true);
            break;
        case application::VerificationDecision::Stop:
            setResolvedState(tr("Run stopped"), false);
            break;
    }
    emit decisionMade(decision, evidence);
}

void VerificationConfirmationWidget::setResolvedState(const QString& status,
                                                      bool accepted)
{
    resolved_ = true;
    evidenceInput_->setEnabled(false);
    evidenceInput_->setVisible(false);
    submitEvidenceButton_->setEnabled(false);
    acceptButton_->setEnabled(false);
    stopButton_->setEnabled(false);
    submitEvidenceButton_->setVisible(false);
    acceptButton_->setVisible(false);
    stopButton_->setVisible(false);
    reasonLabel_->setVisible(false);
    evidenceLabel_->setVisible(false);
    statusLabel_->setText(status);
    statusLabel_->setProperty("accepted", accepted);
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
    statusLabel_->setVisible(true);
}
}  // namespace qtllm::ui
