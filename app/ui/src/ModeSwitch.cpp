#include "ModeSwitch.hpp"

#include <QPaintEvent>
#include <QPainter>

namespace qtllm::ui
{
ModeSwitch::ModeSwitch(QWidget* parent) : QCheckBox(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setMinimumHeight(30);
}

QSize ModeSwitch::sizeHint() const
{
    const auto textWidth = qMax(fontMetrics().horizontalAdvance(tr("Chat")),
                                fontMetrics().horizontalAdvance(text()));
    return {qMax(84, textWidth + 46), 30};
}

QSize ModeSwitch::minimumSizeHint() const
{
    return sizeHint();
}

void ModeSwitch::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF track(1.0, 1.0, width() - 2.0, height() - 2.0);
    const auto radius = track.height() / 2.0;
    QColor background;
    if (!isEnabled())
        background = QColor(QStringLiteral("#cbd5e1"));
    else if (isChecked())
        background = QColor(underMouse() ? QStringLiteral("#1d4ed8")
                                         : QStringLiteral("#2563eb"));
    else
        background = QColor(underMouse() ? QStringLiteral("#0f766e")
                                         : QStringLiteral("#0d9488"));

    painter.setPen(
        QPen(hasFocus() ? QColor(QStringLiteral("#93c5fd")) : background,
             hasFocus() ? 2.0 : 1.0));
    painter.setBrush(background);
    painter.drawRoundedRect(track, radius, radius);

    constexpr qreal knobMargin = 4.0;
    const auto knobDiameter = track.height() - knobMargin * 2.0;
    const auto knobArea = knobDiameter + knobMargin * 2.0;
    const auto knobX = isChecked() ? track.left() + knobMargin
                                   : track.right() - knobMargin - knobDiameter;
    painter.setPen(Qt::NoPen);
    painter.setBrush(isEnabled() ? Qt::white
                                 : QColor(QStringLiteral("#e2e8f0")));
    painter.drawEllipse(
        QRectF(knobX, track.top() + knobMargin, knobDiameter, knobDiameter));

    const QRectF textRect =
        isChecked() ? QRectF(track.left() + knobArea, track.top(),
                             track.width() - knobArea, track.height())
                    : QRectF(track.left(), track.top(),
                             track.width() - knobArea, track.height());
    painter.setPen(Qt::white);
    painter.drawText(textRect, Qt::AlignCenter,
                     isChecked() ? text() : tr("Chat"));
}
}  // namespace qtllm::ui
