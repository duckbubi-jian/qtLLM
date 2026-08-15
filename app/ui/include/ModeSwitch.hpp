#pragma once

#include <QCheckBox>

class QPaintEvent;

namespace qtllm::ui
{
class ModeSwitch final : public QCheckBox
{
    Q_OBJECT

   public:
    explicit ModeSwitch(QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

   protected:
    void paintEvent(QPaintEvent* event) override;
};
}  // namespace qtllm::ui
