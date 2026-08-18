#include "AgentProgressWidget.hpp"

#include "SensitiveData.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace qtllm::ui
{
namespace
{
bool isTerminal(application::AgentRun::State state)
{
    return state == application::AgentRun::State::Completed ||
           state == application::AgentRun::State::Cancelled ||
           state == application::AgentRun::State::Failed;
}

QString stepStatusName(application::AgentProgressStepStatus status)
{
    using application::AgentProgressStepStatus;
    switch (status)
    {
        case AgentProgressStepStatus::Current:
            return QStringLiteral("current");
        case AgentProgressStepStatus::Completed:
            return QStringLiteral("completed");
        case AgentProgressStepStatus::Blocked:
            return QStringLiteral("blocked");
        case AgentProgressStepStatus::Pending:
        default:
            return QStringLiteral("pending");
    }
}

QStyle::StandardPixmap stepIcon(application::AgentProgressStepStatus status)
{
    using application::AgentProgressStepStatus;
    switch (status)
    {
        case AgentProgressStepStatus::Current:
            return QStyle::SP_ArrowRight;
        case AgentProgressStepStatus::Completed:
            return QStyle::SP_DialogApplyButton;
        case AgentProgressStepStatus::Blocked:
            return QStyle::SP_MessageBoxCritical;
        case AgentProgressStepStatus::Pending:
        default:
            return QStyle::SP_DialogResetButton;
    }
}

void clearWidgets(QVBoxLayout* layout)
{
    while (auto* item = layout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
}

QString elapsedText(qint64 milliseconds)
{
    const auto totalSeconds = qMax<qint64>(0, milliseconds / 1'000);
    const auto hours = totalSeconds / 3'600;
    const auto minutes = totalSeconds % 3'600 / 60;
    const auto seconds = totalSeconds % 60;
    return hours > 0 ? QStringLiteral("%1:%2:%3")
                           .arg(hours, 2, 10, QLatin1Char('0'))
                           .arg(minutes, 2, 10, QLatin1Char('0'))
                           .arg(seconds, 2, 10, QLatin1Char('0'))
                     : QStringLiteral("%1:%2")
                           .arg(minutes, 2, 10, QLatin1Char('0'))
                           .arg(seconds, 2, 10, QLatin1Char('0'));
}
}  // namespace

AgentProgressWidget::AgentProgressWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("agentProgressCard"));
    setProperty("agentProgress", true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMaximumWidth(900);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    detailsToggle_ = new QToolButton(this);
    detailsToggle_->setObjectName(QStringLiteral("agentProgressToggle"));
    detailsToggle_->setCheckable(true);
    detailsToggle_->setChecked(true);
    detailsToggle_->setArrowType(Qt::DownArrow);
    detailsToggle_->setToolTip(tr("Collapse agent progress details"));
    detailsToggle_->setAccessibleName(tr("Agent progress details"));
    header->addWidget(detailsToggle_);
    titleLabel_ = new QLabel(tr("Agent working"), this);
    titleLabel_->setObjectName(QStringLiteral("agentProgressTitle"));
    header->addWidget(titleLabel_, 1);
    elapsedLabel_ = new QLabel(QStringLiteral("00:00"), this);
    elapsedLabel_->setObjectName(QStringLiteral("agentProgressElapsed"));
    header->addWidget(elapsedLabel_);
    layout->addLayout(header);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setObjectName(QStringLiteral("agentProgressSummary"));
    summaryLabel_->setWordWrap(true);
    summaryLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(summaryLabel_);

    details_ = new QWidget(this);
    details_->setObjectName(QStringLiteral("agentProgressDetails"));
    auto* detailsLayout = new QVBoxLayout(details_);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(7);

    plan_ = new QWidget(details_);
    plan_->setObjectName(QStringLiteral("agentProgressPlan"));
    planLayout_ = new QVBoxLayout(plan_);
    planLayout_->setContentsMargins(0, 0, 0, 0);
    planLayout_->setSpacing(4);
    detailsLayout->addWidget(plan_);

    operationLabel_ = new QLabel(details_);
    operationLabel_->setObjectName(QStringLiteral("agentProgressOperation"));
    operationLabel_->setWordWrap(true);
    operationLabel_->setSizePolicy(QSizePolicy::Ignored,
                                   QSizePolicy::Preferred);
    detailsLayout->addWidget(operationLabel_);
    waitingLabel_ = new QLabel(details_);
    waitingLabel_->setObjectName(QStringLiteral("agentProgressWaiting"));
    waitingLabel_->setWordWrap(true);
    waitingLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    detailsLayout->addWidget(waitingLabel_);
    countsLabel_ = new QLabel(details_);
    countsLabel_->setObjectName(QStringLiteral("agentProgressCounts"));
    detailsLayout->addWidget(countsLabel_);
    finishLabel_ = new QLabel(details_);
    finishLabel_->setObjectName(QStringLiteral("agentProgressFinish"));
    finishLabel_->setWordWrap(true);
    finishLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    finishLabel_->setVisible(false);
    detailsLayout->addWidget(finishLabel_);

    activityToggle_ = new QToolButton(details_);
    activityToggle_->setObjectName(
        QStringLiteral("agentProgressActivityToggle"));
    activityToggle_->setCheckable(true);
    activityToggle_->setChecked(false);
    activityToggle_->setArrowType(Qt::RightArrow);
    activityToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    activityToggle_->setAccessibleName(tr("Recent agent activity"));
    detailsLayout->addWidget(activityToggle_, 0, Qt::AlignLeft);

    activity_ = new QWidget(details_);
    activity_->setObjectName(QStringLiteral("agentProgressActivity"));
    activityLayout_ = new QVBoxLayout(activity_);
    activityLayout_->setContentsMargins(22, 0, 0, 0);
    activityLayout_->setSpacing(4);
    activity_->setVisible(false);
    detailsLayout->addWidget(activity_);
    layout->addWidget(details_);

    elapsedTimer_ = new QTimer(this);
    elapsedTimer_->setObjectName(QStringLiteral("agentProgressElapsedTimer"));
    elapsedTimer_->setInterval(1'000);
    connect(elapsedTimer_, &QTimer::timeout, this,
            &AgentProgressWidget::refreshElapsed);
    connect(detailsToggle_, &QToolButton::toggled, this,
            [this](bool expanded) { setExpanded(expanded); });
    connect(activityToggle_, &QToolButton::toggled, this,
            [this](bool expanded)
            {
                activityToggle_->setArrowType(expanded ? Qt::DownArrow
                                                       : Qt::RightArrow);
                activity_->setVisible(expanded);
            });
    refresh();
}

void AgentProgressWidget::setSnapshot(
    const application::AgentProgressSnapshot& snapshot)
{
    if (terminal_ && snapshot.runId == snapshot_.runId &&
        !isTerminal(snapshot.state))
        return;

    const auto wasTerminal = terminal_;
    snapshot_ = snapshot;
    elapsedBaseMilliseconds_ = snapshot.elapsedMilliseconds;
    elapsedClock_.restart();
    terminal_ = isTerminal(snapshot.state);
    if (terminal_)
        elapsedTimer_->stop();
    else if (!snapshot.runId.isEmpty())
        elapsedTimer_->start();
    if (terminal_ && !wasTerminal) detailsToggle_->setChecked(false);
    refresh();
}

const application::AgentProgressSnapshot& AgentProgressWidget::snapshot() const
{
    return snapshot_;
}

bool AgentProgressWidget::isExpanded() const
{
    return detailsToggle_->isChecked();
}

void AgentProgressWidget::setExpanded(bool expanded)
{
    detailsToggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    detailsToggle_->setToolTip(expanded ? tr("Collapse agent progress details")
                                        : tr("Expand agent progress details"));
    details_->setVisible(expanded);
}

void AgentProgressWidget::refresh()
{
    using application::AgentProgressStepStatus;
    auto completedSteps = 0;
    auto blockedSteps = 0;
    for (const auto& step : snapshot_.steps)
    {
        if (step.status == AgentProgressStepStatus::Completed) ++completedSteps;
        if (step.status == AgentProgressStepStatus::Blocked) ++blockedSteps;
    }

    switch (snapshot_.state)
    {
        case application::AgentRun::State::Completed:
            titleLabel_->setText(blockedSteps > 0 ? tr("Agent blocked")
                                                  : tr("Agent complete"));
            break;
        case application::AgentRun::State::Cancelled:
            titleLabel_->setText(tr("Agent stopped"));
            break;
        case application::AgentRun::State::Failed:
            titleLabel_->setText(tr("Agent failed"));
            break;
        default:
            titleLabel_->setText(tr("Agent working"));
            break;
    }
    setProperty("progressState", terminal_ ? QStringLiteral("terminal")
                                           : QStringLiteral("active"));
    style()->unpolish(this);
    style()->polish(this);

    summaryLabel_->setText(snapshot_.steps.isEmpty()
                               ? tr("No structured plan")
                               : tr("Plan: %1 steps, %2 complete")
                                     .arg(snapshot_.steps.size())
                                     .arg(completedSteps));
    operationLabel_->setText(
        snapshot_.operation.isEmpty()
            ? QString{}
            : tr("Current: %1")
                  .arg(redactSensitiveText(snapshot_.operation, 240)));
    operationLabel_->setVisible(!snapshot_.operation.isEmpty());
    waitingLabel_->setText(redactSensitiveText(snapshot_.waitingReason, 240));
    waitingLabel_->setVisible(!snapshot_.waitingReason.isEmpty());
    countsLabel_->setText(tr("%1 tools completed | %2 evidence items")
                              .arg(snapshot_.completedToolCount)
                              .arg(snapshot_.evidenceCount));
    const auto finishText = redactSensitiveText(
        snapshot_.finishMessage.isEmpty() ? snapshot_.finishCode
                                          : snapshot_.finishMessage,
        320);
    finishLabel_->setText(finishText);
    finishLabel_->setVisible(terminal_ && !finishText.isEmpty());
    refreshPlan();
    refreshActivities();
    refreshElapsed();
}

void AgentProgressWidget::refreshPlan()
{
    clearWidgets(planLayout_);
    plan_->setVisible(!snapshot_.steps.isEmpty());
    for (const auto& step : snapshot_.steps)
    {
        auto* row = new QWidget(plan_);
        row->setObjectName(QStringLiteral("agentProgressStep"));
        const auto statusName = stepStatusName(step.status);
        const auto descriptionText = redactSensitiveText(step.description, 256);
        row->setProperty("stepStatus", statusName);
        row->setAccessibleName(tr("%1: %2").arg(statusName, descriptionText));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(2, 1, 2, 1);
        rowLayout->setSpacing(7);
        auto* icon = new QLabel(row);
        icon->setObjectName(QStringLiteral("agentProgressStepIcon"));
        icon->setFixedSize(18, 18);
        icon->setPixmap(
            style()->standardIcon(stepIcon(step.status)).pixmap(16, 16));
        rowLayout->addWidget(icon, 0, Qt::AlignTop);
        auto* description = new QLabel(descriptionText, row);
        description->setObjectName(
            QStringLiteral("agentProgressStepDescription"));
        description->setWordWrap(true);
        description->setSizePolicy(QSizePolicy::Ignored,
                                   QSizePolicy::Preferred);
        rowLayout->addWidget(description, 1);
        planLayout_->addWidget(row);
    }
}

void AgentProgressWidget::refreshActivities()
{
    clearWidgets(activityLayout_);
    activityToggle_->setText(
        tr("Recent activity (%1)").arg(snapshot_.recentActivity.size()));
    activityToggle_->setVisible(!snapshot_.recentActivity.isEmpty());
    if (snapshot_.recentActivity.isEmpty())
    {
        const QSignalBlocker blocker(activityToggle_);
        activityToggle_->setChecked(false);
        activity_->setVisible(false);
        return;
    }

    for (const auto& item : snapshot_.recentActivity)
    {
        const auto time = item.timestamp.isValid()
                              ? item.timestamp.toLocalTime().toString(
                                    QStringLiteral("HH:mm:ss"))
                              : QStringLiteral("--:--:--");
        auto detail = item.message.isEmpty() ? agent::eventTypeName(item.type)
                                             : item.message;
        if (!item.toolName.isEmpty())
            detail += QStringLiteral(" [%1]").arg(item.toolName);
        auto* label = new QLabel(QStringLiteral("%1  %2").arg(
                                     time, redactSensitiveText(detail, 280)),
                                 activity_);
        label->setObjectName(QStringLiteral("agentProgressActivityItem"));
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        activityLayout_->addWidget(label);
    }
}

void AgentProgressWidget::refreshElapsed()
{
    const auto elapsed =
        elapsedBaseMilliseconds_ +
        ((!terminal_ && elapsedClock_.isValid()) ? elapsedClock_.elapsed() : 0);
    elapsedLabel_->setText(elapsedText(elapsed));
}
}  // namespace qtllm::ui
