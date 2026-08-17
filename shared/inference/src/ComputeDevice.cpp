#include "ComputeDevice.hpp"

#include <QSet>

#include <cmath>

namespace qtllm::inference
{
QString devicePlacementModeName(DevicePlacementMode mode)
{
    switch (mode)
    {
        case DevicePlacementMode::Auto:
            return QStringLiteral("auto");
        case DevicePlacementMode::Cpu:
            return QStringLiteral("cpu");
        case DevicePlacementMode::Single:
            return QStringLiteral("single");
        case DevicePlacementMode::Custom:
            return QStringLiteral("custom");
    }
    return QStringLiteral("auto");
}

bool parseDevicePlacementMode(const QString& name, DevicePlacementMode& mode)
{
    if (name == QLatin1String("auto"))
        mode = DevicePlacementMode::Auto;
    else if (name == QLatin1String("cpu"))
        mode = DevicePlacementMode::Cpu;
    else if (name == QLatin1String("single"))
        mode = DevicePlacementMode::Single;
    else if (name == QLatin1String("custom"))
        mode = DevicePlacementMode::Custom;
    else
        return false;
    return true;
}

bool validateModelLoadOptions(const ModelLoadOptions& options,
                              QString& errorMessage)
{
    if (options.gpuLayers < -1 || options.gpuLayers > 10'000)
    {
        errorMessage =
            QStringLiteral("gpuLayers must be between -1 and 10000.");
        return false;
    }

    if (options.placementMode == DevicePlacementMode::Auto ||
        options.placementMode == DevicePlacementMode::Cpu)
    {
        if (!options.devices.isEmpty())
        {
            errorMessage = QStringLiteral(
                "Auto and CPU placement must not select explicit devices.");
            return false;
        }
        return true;
    }

    const auto requiredDevices =
        options.placementMode == DevicePlacementMode::Single ? 1 : 2;
    if (options.devices.size() < requiredDevices ||
        (options.placementMode == DevicePlacementMode::Single &&
         options.devices.size() != 1))
    {
        errorMessage =
            options.placementMode == DevicePlacementMode::Single
                ? QStringLiteral("Single placement requires exactly one GPU.")
                : QStringLiteral(
                      "Custom placement requires at least two GPUs.");
        return false;
    }

    QSet<QString> deviceIds;
    for (const auto& device : options.devices)
    {
        if (device.id.trimmed().isEmpty())
        {
            errorMessage =
                QStringLiteral("Selected GPU IDs must not be empty.");
            return false;
        }
        if (deviceIds.contains(device.id))
        {
            errorMessage = QStringLiteral("Selected GPU IDs must be unique.");
            return false;
        }
        if (!std::isfinite(device.weight) || device.weight <= 0.0F)
        {
            errorMessage =
                QStringLiteral("Selected GPU weights must be positive.");
            return false;
        }
        deviceIds.insert(device.id);
    }
    return true;
}
}  // namespace qtllm::inference
