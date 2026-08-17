#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

namespace qtllm::inference
{
enum class DevicePlacementMode
{
    Auto,
    Cpu,
    Single,
    Custom
};

struct ComputeDevice
{
    QString id;
    QString backendName;
    QString description;
    QString hardwareId;
    quint64 freeMemoryBytes = 0;
    quint64 totalMemoryBytes = 0;

    friend bool operator==(const ComputeDevice&,
                           const ComputeDevice&) = default;
};

struct DeviceSelection
{
    QString id;
    float weight = 1.0F;

    friend bool operator==(const DeviceSelection&,
                           const DeviceSelection&) = default;
};

struct ModelLoadOptions
{
    int gpuLayers = -1;
    DevicePlacementMode placementMode = DevicePlacementMode::Auto;
    QList<DeviceSelection> devices;

    friend bool operator==(const ModelLoadOptions&,
                           const ModelLoadOptions&) = default;
};

[[nodiscard]] QString devicePlacementModeName(DevicePlacementMode mode);
bool parseDevicePlacementMode(const QString& name, DevicePlacementMode& mode);
bool validateModelLoadOptions(const ModelLoadOptions& options,
                              QString& errorMessage);
}  // namespace qtllm::inference

Q_DECLARE_METATYPE(qtllm::inference::ComputeDevice)
Q_DECLARE_METATYPE(QList<qtllm::inference::ComputeDevice>)
