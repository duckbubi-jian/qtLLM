#include "ComputeSettingsDialog.hpp"

#include "ui_ComputeSettingsDialog.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>

#include <algorithm>

namespace qtllm::ui
{
namespace
{
QString memoryDescription(const inference::ComputeDevice& device)
{
    constexpr auto gibibyte = 1024.0 * 1024.0 * 1024.0;
    return QStringLiteral("%1 / %2 GiB free")
        .arg(static_cast<double>(device.freeMemoryBytes) / gibibyte, 0, 'f', 1)
        .arg(static_cast<double>(device.totalMemoryBytes) / gibibyte, 0, 'f',
             1);
}
}  // namespace

ComputeSettingsDialog::ComputeSettingsDialog(
    const QList<inference::ComputeDevice>& devices, QWidget* parent)
    : QDialog(parent),
      ui_(std::make_unique<Ui::ComputeSettingsDialog>()),
      devices_(devices)
{
    ui_->setupUi(this);
    ui_->placementModeCombo->addItem(
        tr("Automatic"),
        static_cast<int>(inference::DevicePlacementMode::Auto));
    ui_->placementModeCombo->addItem(
        tr("CPU only"), static_cast<int>(inference::DevicePlacementMode::Cpu));
    ui_->placementModeCombo->addItem(
        tr("Single GPU"),
        static_cast<int>(inference::DevicePlacementMode::Single));
    ui_->placementModeCombo->addItem(
        tr("Custom split"),
        static_cast<int>(inference::DevicePlacementMode::Custom));
    ui_->gpuLayersSpin->setRange(-1, 10'000);
    ui_->gpuLayersSpin->setSpecialValueText(tr("All layers"));

    ui_->deviceTable->setRowCount(devices_.size());
    ui_->deviceTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    ui_->deviceTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    ui_->deviceTable->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    ui_->deviceTable->verticalHeader()->setVisible(false);
    ui_->deviceTable->setSelectionMode(QAbstractItemView::NoSelection);

    for (qsizetype row = 0; row < devices_.size(); ++row)
    {
        const auto& device = devices_.at(row);
        auto* deviceItem = new QTableWidgetItem(QStringLiteral("%1 (%2)").arg(
            device.description, device.backendName));
        deviceItem->setData(Qt::UserRole, device.id);
        deviceItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        deviceItem->setCheckState(Qt::Checked);
        ui_->deviceTable->setItem(row, 0, deviceItem);

        auto* memoryItem = new QTableWidgetItem(memoryDescription(device));
        memoryItem->setFlags(Qt::ItemIsEnabled);
        ui_->deviceTable->setItem(row, 1, memoryItem);

        auto* weight = new QDoubleSpinBox(ui_->deviceTable);
        weight->setRange(0.01, 100.0);
        weight->setDecimals(2);
        weight->setSingleStep(0.25);
        weight->setValue(1.0);
        weight->setAlignment(Qt::AlignRight);
        ui_->deviceTable->setCellWidget(row, 2, weight);
    }

    connect(ui_->placementModeCombo, &QComboBox::currentIndexChanged, this,
            &ComputeSettingsDialog::updateModeControls);
    connect(ui_->deviceTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem* changedItem)
            {
                if (selectedMode() != inference::DevicePlacementMode::Single ||
                    changedItem->column() != 0 ||
                    changedItem->checkState() != Qt::Checked)
                    return;
                const QSignalBlocker blocker(ui_->deviceTable);
                for (auto row = 0; row < ui_->deviceTable->rowCount(); ++row)
                {
                    auto* item = ui_->deviceTable->item(row, 0);
                    if (item != changedItem) item->setCheckState(Qt::Unchecked);
                }
            });
    connect(ui_->buttonBox, &QDialogButtonBox::accepted, this,
            &ComputeSettingsDialog::accept);
    connect(ui_->buttonBox, &QDialogButtonBox::rejected, this,
            &ComputeSettingsDialog::reject);
    setOptions({});
}

ComputeSettingsDialog::~ComputeSettingsDialog() = default;

void ComputeSettingsDialog::setOptions(
    const inference::ModelLoadOptions& options)
{
    ui_->gpuLayersSpin->setValue(options.gpuLayers);
    const auto modeIndex = ui_->placementModeCombo->findData(
        static_cast<int>(options.placementMode));
    ui_->placementModeCombo->setCurrentIndex(qMax(0, modeIndex));

    auto matchedDevices = 0;
    for (auto row = 0; row < ui_->deviceTable->rowCount(); ++row)
    {
        auto* item = ui_->deviceTable->item(row, 0);
        const auto id = item->data(Qt::UserRole).toString();
        const auto match =
            std::find_if(options.devices.cbegin(), options.devices.cend(),
                         [&id](const inference::DeviceSelection& selection)
                         { return selection.id == id; });
        item->setCheckState(match == options.devices.cend() ? Qt::Unchecked
                                                            : Qt::Checked);
        if (match != options.devices.cend())
        {
            ++matchedDevices;
            auto* weight = qobject_cast<QDoubleSpinBox*>(
                ui_->deviceTable->cellWidget(row, 2));
            weight->setValue(match->weight);
        }
    }

    if ((options.placementMode == inference::DevicePlacementMode::Single ||
         options.placementMode == inference::DevicePlacementMode::Custom) &&
        matchedDevices != options.devices.size())
    {
        ui_->placementModeCombo->setCurrentIndex(
            ui_->placementModeCombo->findData(
                static_cast<int>(inference::DevicePlacementMode::Auto)));
        showValidationError(tr(
            "Saved GPU selection is unavailable. Automatic mode is active."));
    }
    else
    {
        ui_->validationLabel->clear();
        ui_->validationLabel->hide();
    }
    updateModeControls();
}

inference::ModelLoadOptions ComputeSettingsDialog::options() const
{
    return acceptedOptions_;
}

void ComputeSettingsDialog::accept()
{
    const auto candidate = currentOptions();
    QString errorMessage;
    if (!inference::validateModelLoadOptions(candidate, errorMessage))
    {
        showValidationError(errorMessage);
        return;
    }
    acceptedOptions_ = candidate;
    QDialog::accept();
}

void ComputeSettingsDialog::updateModeControls()
{
    const auto mode = selectedMode();
    const auto explicitSelection =
        mode == inference::DevicePlacementMode::Single ||
        mode == inference::DevicePlacementMode::Custom;
    const auto custom = mode == inference::DevicePlacementMode::Custom;
    ui_->gpuLayersSpin->setEnabled(mode != inference::DevicePlacementMode::Cpu);
    ui_->deviceTable->setEnabled(explicitSelection);

    auto checkedRows = 0;
    auto firstCheckedRow = -1;
    for (auto row = 0; row < ui_->deviceTable->rowCount(); ++row)
    {
        auto* item = ui_->deviceTable->item(row, 0);
        if (mode == inference::DevicePlacementMode::Auto)
            item->setCheckState(Qt::Checked);
        else if (mode == inference::DevicePlacementMode::Cpu)
            item->setCheckState(Qt::Unchecked);
        if (item->checkState() == Qt::Checked)
        {
            if (firstCheckedRow < 0) firstCheckedRow = row;
            ++checkedRows;
        }
        ui_->deviceTable->cellWidget(row, 2)->setEnabled(custom);
    }
    if (explicitSelection && checkedRows == 0 &&
        ui_->deviceTable->rowCount() > 0)
    {
        ui_->deviceTable->item(0, 0)->setCheckState(Qt::Checked);
        checkedRows = 1;
        firstCheckedRow = 0;
    }
    if (mode == inference::DevicePlacementMode::Single && checkedRows > 1)
    {
        for (auto row = 0; row < ui_->deviceTable->rowCount(); ++row)
        {
            if (row != firstCheckedRow)
                ui_->deviceTable->item(row, 0)->setCheckState(Qt::Unchecked);
        }
        checkedRows = 1;
    }
    if (custom && checkedRows < 2)
    {
        for (auto row = 0;
             row < ui_->deviceTable->rowCount() && checkedRows < 2; ++row)
        {
            auto* item = ui_->deviceTable->item(row, 0);
            if (item->checkState() == Qt::Checked) continue;
            item->setCheckState(Qt::Checked);
            ++checkedRows;
        }
    }
}

inference::DevicePlacementMode ComputeSettingsDialog::selectedMode() const
{
    return static_cast<inference::DevicePlacementMode>(
        ui_->placementModeCombo->currentData().toInt());
}

inference::ModelLoadOptions ComputeSettingsDialog::currentOptions() const
{
    inference::ModelLoadOptions result;
    result.gpuLayers = ui_->gpuLayersSpin->value();
    result.placementMode = selectedMode();
    if (result.placementMode != inference::DevicePlacementMode::Single &&
        result.placementMode != inference::DevicePlacementMode::Custom)
        return result;

    for (auto row = 0; row < ui_->deviceTable->rowCount(); ++row)
    {
        const auto* item = ui_->deviceTable->item(row, 0);
        if (item->checkState() != Qt::Checked) continue;
        const auto* weight =
            qobject_cast<QDoubleSpinBox*>(ui_->deviceTable->cellWidget(row, 2));
        result.devices.append(
            {item->data(Qt::UserRole).toString(),
             result.placementMode == inference::DevicePlacementMode::Custom
                 ? static_cast<float>(weight->value())
                 : 1.0F});
    }
    return result;
}

void ComputeSettingsDialog::showValidationError(const QString& message)
{
    ui_->validationLabel->setText(message);
    ui_->validationLabel->show();
}
}  // namespace qtllm::ui
