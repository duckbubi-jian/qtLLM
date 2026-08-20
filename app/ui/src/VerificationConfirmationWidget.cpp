#include "VerificationConfirmationWidget.hpp"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
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
QStringList jsonStrings(const QJsonObject& context, const QString& key)
{
    QStringList result;
    for (const auto& value : context.value(key).toArray())
    {
        const auto text = value.toString().trimmed();
        if (!text.isEmpty() && !result.contains(text)) result.append(text);
    }
    return result;
}

QString jsonValueText(const QJsonValue& value)
{
    auto serialized = QString::fromUtf8(
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
    if (serialized.startsWith(QLatin1Char('[')) &&
        serialized.endsWith(QLatin1Char(']')))
        serialized = serialized.mid(1, serialized.size() - 2);
    constexpr auto maximumDisplayedCharacters = 512;
    if (serialized.size() > maximumDisplayedCharacters)
        serialized =
            serialized.left(maximumDisplayedCharacters) + QStringLiteral("...");
    return serialized;
}

QString expectedEffectsText(const QJsonObject& context)
{
    QStringList effects;
    for (const auto& value :
         context.value(QStringLiteral("expectedEffects")).toArray())
    {
        const auto object = value.toObject();
        const auto field = object.value(QStringLiteral("field")).toString();
        if (field.isEmpty()) continue;
        const auto expectedValue =
            jsonValueText(object.value(QStringLiteral("value")));
        effects.append(QStringLiteral("%1 = %2").arg(field, expectedValue));
    }
    if (!effects.isEmpty()) return effects.join(QLatin1Char('\n'));
    return jsonStrings(context, QStringLiteral("expectedEffectFields"))
        .join(QStringLiteral(", "));
}

QString problemText(const QJsonObject& context, const QString& fallback)
{
    const auto mismatched =
        jsonStrings(context, QStringLiteral("mismatchedEffectFields"));
    if (!mismatched.isEmpty())
        return VerificationConfirmationWidget::tr(
                   "Read-back returned different values for: %1")
            .arg(mismatched.join(QStringLiteral(", ")));

    const auto missing =
        jsonStrings(context, QStringLiteral("missingEffectFields"));
    if (!missing.isEmpty())
        return VerificationConfirmationWidget::tr(
                   "Read-back did not return: %1")
            .arg(missing.join(QStringLiteral(", ")));

    const auto state = context.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("uncertain"))
        return VerificationConfirmationWidget::tr(
            "The operation was dispatched, but its final outcome is unknown.");
    if (state == QLatin1String("unavailable"))
        return VerificationConfirmationWidget::tr(
            "The operation has no stable target identifier for read-back.");
    if (state == QLatin1String("failed"))
        return VerificationConfirmationWidget::tr(
            "The read-back operation failed.");

    const auto detail = context.value(QStringLiteral("detail")).toString();
    return detail.isEmpty() ? fallback : detail;
}

void addDetailRow(QGridLayout* details, int row, const QString& title,
                  const QString& value, const QString& objectName,
                  QWidget* parent)
{
    if (value.isEmpty()) return;
    auto* titleLabel = new QLabel(title, parent);
    titleLabel->setObjectName(QStringLiteral("verificationDetailTitle"));
    titleLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    details->addWidget(titleLabel, row, 0);

    auto* valueLabel = new QLabel(value, parent);
    valueLabel->setObjectName(objectName);
    valueLabel->setWordWrap(true);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(valueLabel, row, 1);
}
}  // namespace

VerificationConfirmationWidget::VerificationConfirmationWidget(
    const QString& reason, const QJsonObject& context, QWidget* parent)
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

    detailsContainer_ = new QWidget(this);
    auto* details = new QGridLayout(detailsContainer_);
    details->setContentsMargins(0, 1, 0, 1);
    details->setHorizontalSpacing(12);
    details->setVerticalSpacing(5);
    details->setColumnStretch(1, 1);

    auto task = context.value(QStringLiteral("taskDescription")).toString();
    const auto stepNumber = context.value(QStringLiteral("stepNumber")).toInt();
    const auto stepCount = context.value(QStringLiteral("stepCount")).toInt();
    if (stepNumber > 0 && stepCount > 0)
        task = QStringLiteral("%1 of %2: %3")
                   .arg(stepNumber)
                   .arg(stepCount)
                   .arg(task);

    auto operation = context.value(QStringLiteral("tool")).toString();
    const auto evidenceSequence =
        context.value(QStringLiteral("mutationEvidenceSequence")).toInt();
    if (evidenceSequence > 0)
        operation =
            operation.isEmpty()
                ? tr("Mutation evidence #%1").arg(evidenceSequence)
                : operation +
                      tr(" | mutation evidence #%1").arg(evidenceSequence);

    QStringList targetParts;
    const auto targetNames =
        jsonStrings(context, QStringLiteral("targetNames"));
    const auto targetIds = jsonStrings(context, QStringLiteral("targetIds"));
    if (!targetNames.isEmpty())
        targetParts.append(
            tr("Name: %1").arg(targetNames.join(QStringLiteral(", "))));
    if (!targetIds.isEmpty())
        targetParts.append(
            tr("ID: %1").arg(targetIds.join(QStringLiteral(", "))));

    const auto expected = expectedEffectsText(context);
    auto problem = problemText(context, reason);
    if (problem.isEmpty())
        problem = tr("Host could not deterministically verify this operation.");
    addDetailRow(details, 0, tr("Current step"), task,
                 QStringLiteral("verificationStep"), detailsContainer_);
    addDetailRow(details, 1, tr("Operation"), operation,
                 QStringLiteral("verificationOperation"), detailsContainer_);
    addDetailRow(details, 2, tr("Target"),
                 targetParts.join(QStringLiteral("\n")),
                 QStringLiteral("verificationTarget"), detailsContainer_);
    addDetailRow(details, 3, tr("Problem"), problem,
                 QStringLiteral("verificationReason"), detailsContainer_);
    addDetailRow(details, 4, tr("Please check"), expected,
                 QStringLiteral("verificationExpectedEffects"),
                 detailsContainer_);
    layout->addWidget(detailsContainer_);

    evidenceLabel_ = new QLabel(tr("Temporary evidence"), this);
    evidenceLabel_->setObjectName(QStringLiteral("temporaryEvidenceLabel"));
    layout->addWidget(evidenceLabel_);

    evidenceInput_ = new QPlainTextEdit(this);
    evidenceInput_->setObjectName(QStringLiteral("temporaryEvidenceInput"));
    evidenceInput_->setPlaceholderText(
        expected.isEmpty()
            ? tr("State where you checked the result and what you observed")
            : tr("State where you checked these fields and the values you "
                 "observed"));
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
    detailsContainer_->setVisible(false);
    evidenceLabel_->setVisible(false);
    statusLabel_->setText(status);
    statusLabel_->setProperty("accepted", accepted);
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
    statusLabel_->setVisible(true);
}
}  // namespace qtllm::ui
