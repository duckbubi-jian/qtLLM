#pragma once

#include "ComputeDevice.hpp"

#include <QDialog>
#include <QList>

#include <memory>

namespace Ui
{
class ComputeSettingsDialog;
}

namespace qtllm::ui
{
class ComputeSettingsDialog final : public QDialog
{
    Q_OBJECT

   public:
    explicit ComputeSettingsDialog(
        const QList<inference::ComputeDevice>& devices,
        QWidget* parent = nullptr);
    ~ComputeSettingsDialog() override;

    void setOptions(const inference::ModelLoadOptions& options);
    [[nodiscard]] inference::ModelLoadOptions options() const;

   public slots:
    void accept() override;

   private:
    void updateModeControls();
    [[nodiscard]] inference::DevicePlacementMode selectedMode() const;
    [[nodiscard]] inference::ModelLoadOptions currentOptions() const;
    void showValidationError(const QString& message);

    std::unique_ptr<Ui::ComputeSettingsDialog> ui_;
    QList<inference::ComputeDevice> devices_;
    inference::ModelLoadOptions acceptedOptions_;
};
}  // namespace qtllm::ui
