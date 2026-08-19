#include "AgentProgressWidget.hpp"

#include "SensitiveData.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace qtllm::ui
{
namespace
{
class ElidedLabel final : public QLabel
{
   public:
    explicit ElidedLabel(QWidget* parent = nullptr) : QLabel(parent)
    {
    }

    void setFullText(const QString& text)
    {
        fullText_ = text;
        setToolTip(fullText_);
        refreshText();
    }

   protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        refreshText();
    }

   private:
    void refreshText()
    {
        setText(width() > 0 ? fontMetrics().elidedText(fullText_,
                                                       Qt::ElideRight, width())
                            : fullText_);
    }

    QString fullText_;
};

bool isTerminal(application::AgentRun::State state)
{
    return state == application::AgentRun::State::Completed ||
           state == application::AgentRun::State::Blocked ||
           state == application::AgentRun::State::Cancelled ||
           state == application::AgentRun::State::Failed;
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
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(6);
    stateToggle_ = new QToolButton(this);
    stateToggle_->setObjectName(QStringLiteral("agentProgressStateToggle"));
    stateToggle_->setCheckable(true);
    stateToggle_->setChecked(true);
    stateToggle_->setArrowType(Qt::DownArrow);
    stateToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    stateToggle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    stateToggle_->setAccessibleName(tr("Agent status and task plan"));
    header->addWidget(stateToggle_, 1);
    elapsedLabel_ = new QLabel(QStringLiteral("00:00"), this);
    elapsedLabel_->setObjectName(QStringLiteral("agentProgressElapsed"));
    header->addWidget(elapsedLabel_);
    layout->addLayout(header);

    plan_ = new QWidget(this);
    plan_->setObjectName(QStringLiteral("agentProgressPlan"));
    plan_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    planLayout_ = new QVBoxLayout(plan_);
    planLayout_->setContentsMargins(6, 3, 6, 3);
    planLayout_->setSpacing(0);
    planLayout_->setSizeConstraint(QLayout::SetMinAndMaxSize);
    plan_->setVisible(false);
    layout->addWidget(plan_);

    finishLabel_ = new QLabel(this);
    finishLabel_->setObjectName(QStringLiteral("agentProgressFinish"));
    finishLabel_->setWordWrap(true);
    finishLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    finishLabel_->setVisible(false);
    layout->addWidget(finishLabel_);

    elapsedTimer_ = new QTimer(this);
    elapsedTimer_->setObjectName(QStringLiteral("agentProgressElapsedTimer"));
    elapsedTimer_->setInterval(1'000);
    connect(elapsedTimer_, &QTimer::timeout, this,
            &AgentProgressWidget::refreshElapsed);
    connect(stateToggle_, &QToolButton::toggled, this,
            [this](bool expanded)
            {
                stateToggle_->setArrowType(expanded ? Qt::DownArrow
                                                    : Qt::RightArrow);
                plan_->setVisible(expanded && !snapshot_.steps.isEmpty());
            });
    refresh();
}

void AgentProgressWidget::setSnapshot(
    const application::AgentProgressSnapshot& snapshot)
{
    if (terminal_ && snapshot.runId == snapshot_.runId &&
        !isTerminal(snapshot.state))
        return;

    const auto hadPlan = !snapshot_.steps.isEmpty();
    const auto wasTerminal = terminal_;
    snapshot_ = snapshot;
    elapsedBaseMilliseconds_ = snapshot.elapsedMilliseconds;
    elapsedClock_.restart();
    terminal_ = isTerminal(snapshot.state);
    if (terminal_)
        elapsedTimer_->stop();
    else if (!snapshot.runId.isEmpty())
        elapsedTimer_->start();
    if (!hadPlan && !snapshot_.steps.isEmpty()) stateToggle_->setChecked(true);
    if (terminal_ && !wasTerminal) stateToggle_->setChecked(false);
    refresh();
}

const application::AgentProgressSnapshot& AgentProgressWidget::snapshot() const
{
    return snapshot_;
}

void AgentProgressWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    refreshStateText();
}

void AgentProgressWidget::refresh()
{
    switch (snapshot_.state)
    {
        case application::AgentRun::State::Completed:
            stateText_ = tr("Agent complete");
            break;
        case application::AgentRun::State::Blocked:
            stateText_ = tr("Agent blocked");
            break;
        case application::AgentRun::State::Cancelled:
            stateText_ = tr("Agent stopped");
            break;
        case application::AgentRun::State::Failed:
            stateText_ = tr("Agent failed");
            break;
        default:
            stateText_ = tr("Agent working");
            break;
    }
    setProperty("progressState", terminal_ ? QStringLiteral("terminal")
                                           : QStringLiteral("active"));
    style()->unpolish(this);
    style()->polish(this);

    const auto operation = snapshot_.operation.isEmpty()
                               ? snapshot_.waitingReason
                               : snapshot_.operation;
    operationText_ = redactSensitiveText(operation, 240);
    refreshStateText();
    const auto finishText = redactSensitiveText(
        snapshot_.finishMessage.isEmpty() ? snapshot_.finishCode
                                          : snapshot_.finishMessage,
        320);
    finishLabel_->setText(finishText);
    finishLabel_->setVisible(
        terminal_ && snapshot_.state != application::AgentRun::State::Blocked &&
        !finishText.isEmpty());
    refreshPlan();
    refreshElapsed();
}

void AgentProgressWidget::refreshStateText()
{
    auto text = stateText_;
    if (!terminal_ && !operationText_.isEmpty())
        text += QStringLiteral(" \u00b7 ") + operationText_;
    const auto availableWidth = stateToggle_->width() - 28;
    stateToggle_->setText(availableWidth > 0
                              ? stateToggle_->fontMetrics().elidedText(
                                    text, Qt::ElideRight, availableWidth)
                              : text);
    stateToggle_->setToolTip(text);
}

void AgentProgressWidget::refreshPlan()
{
    clearWidgets(planLayout_);
    plan_->setVisible(!snapshot_.steps.isEmpty() && stateToggle_->isChecked());
    for (qsizetype index = 0; index < snapshot_.steps.size(); ++index)
    {
        const auto& step = snapshot_.steps.at(index);
        auto* row = new QWidget(plan_);
        row->setObjectName(QStringLiteral("agentProgressStep"));
        const auto descriptionText = redactSensitiveText(step.description, 256);
        const auto displayText =
            QStringLiteral("%1. %2").arg(index + 1).arg(descriptionText);
        row->setAccessibleName(displayText);
        row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        row->setFixedHeight(28);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 0, 8, 0);
        auto* description = new ElidedLabel(row);
        description->setObjectName(
            QStringLiteral("agentProgressStepDescription"));
        description->setFullText(displayText);
        description->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        description->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        description->setFixedHeight(26);
        description->setTextInteractionFlags(Qt::TextSelectableByMouse);
        rowLayout->addWidget(description, 1);
        auto* elapsed = new QLabel(elapsedText(step.elapsedMilliseconds), row);
        elapsed->setObjectName(QStringLiteral("agentProgressStepElapsed"));
        elapsed->setProperty("elapsedBase", step.elapsedMilliseconds);
        elapsed->setProperty(
            "active",
            step.status == application::AgentProgressStepStatus::Current);
        elapsed->setToolTip(redactSensitiveText(step.activity, 160));
        rowLayout->addWidget(elapsed);
        planLayout_->addWidget(row);
    }
}

void AgentProgressWidget::refreshElapsed()
{
    const auto elapsed =
        elapsedBaseMilliseconds_ +
        ((!terminal_ && elapsedClock_.isValid()) ? elapsedClock_.elapsed() : 0);
    elapsedLabel_->setText(elapsedText(elapsed));
    const auto stepElapsedLabels = plan_->findChildren<QLabel*>(
        QStringLiteral("agentProgressStepElapsed"));
    for (auto* label : stepElapsedLabels)
    {
        auto stepElapsed = label->property("elapsedBase").toLongLong();
        if (!terminal_ && label->property("active").toBool() &&
            elapsedClock_.isValid())
            stepElapsed += elapsedClock_.elapsed();
        label->setText(elapsedText(stepElapsed));
    }
}
}  // namespace qtllm::ui
