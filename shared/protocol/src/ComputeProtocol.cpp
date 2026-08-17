#include "ComputeProtocol.hpp"

#include <QJsonValue>

#include <cmath>
#include <limits>
#include <utility>

namespace qtllm::protocol
{
namespace
{
bool readInteger(const QJsonObject& object, const QString& name, qint64 minimum,
                 qint64 maximum, qint64& result, QString& errorMessage)
{
    const auto value = object.value(name);
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble()) != value.toDouble() ||
        value.toDouble() < static_cast<double>(minimum) ||
        value.toDouble() > static_cast<double>(maximum))
    {
        errorMessage =
            QStringLiteral("%1 must be an integer between %2 and %3.")
                .arg(name)
                .arg(minimum)
                .arg(maximum);
        return false;
    }
    result = static_cast<qint64>(value.toDouble());
    return true;
}
}  // namespace

QJsonObject serializeModelLoadOptions(
    const inference::ModelLoadOptions& options)
{
    QJsonArray deviceIds;
    QJsonArray weights;
    for (const auto& device : options.devices)
    {
        deviceIds.append(device.id);
        weights.append(device.weight);
    }
    return {{QStringLiteral("gpuLayers"), options.gpuLayers},
            {QStringLiteral("placement"),
             QJsonObject{
                 {QStringLiteral("mode"),
                  inference::devicePlacementModeName(options.placementMode)},
                 {QStringLiteral("deviceIds"), deviceIds},
                 {QStringLiteral("weights"), weights}}}};
}

bool parseModelLoadOptions(const QJsonObject& object,
                           inference::ModelLoadOptions& options,
                           QString& errorMessage)
{
    options = {};
    qint64 gpuLayers = -1;
    const auto gpuLayersValue = object.value(QStringLiteral("gpuLayers"));
    if (!gpuLayersValue.isUndefined() &&
        !readInteger(object, QStringLiteral("gpuLayers"), -1, 10'000, gpuLayers,
                     errorMessage))
        return false;
    options.gpuLayers = static_cast<int>(gpuLayers);

    const auto placementValue = object.value(QStringLiteral("placement"));
    if (placementValue.isUndefined())
        return inference::validateModelLoadOptions(options, errorMessage);
    if (!placementValue.isObject())
    {
        errorMessage = QStringLiteral("placement must be an object.");
        return false;
    }

    const auto placement = placementValue.toObject();
    const auto modeValue = placement.value(QStringLiteral("mode"));
    if (!modeValue.isString() ||
        !inference::parseDevicePlacementMode(modeValue.toString(),
                                             options.placementMode))
    {
        errorMessage = QStringLiteral(
            "placement.mode must be auto, cpu, single, or custom.");
        return false;
    }

    const auto idsValue = placement.value(QStringLiteral("deviceIds"));
    const auto weightsValue = placement.value(QStringLiteral("weights"));
    if ((!idsValue.isUndefined() && !idsValue.isArray()) ||
        (!weightsValue.isUndefined() && !weightsValue.isArray()))
    {
        errorMessage = QStringLiteral(
            "placement.deviceIds and placement.weights must be arrays.");
        return false;
    }
    const auto ids = idsValue.toArray();
    const auto weights = weightsValue.toArray();
    if (!weights.isEmpty() && weights.size() != ids.size())
    {
        errorMessage =
            QStringLiteral("placement.weights must match placement.deviceIds.");
        return false;
    }
    for (qsizetype index = 0; index < ids.size(); ++index)
    {
        if (!ids.at(index).isString())
        {
            errorMessage = QStringLiteral("placement.deviceIds[%1] is invalid.")
                               .arg(index);
            return false;
        }
        auto weight = 1.0F;
        if (!weights.isEmpty())
        {
            const auto value = weights.at(index);
            if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
                value.toDouble() <= 0.0 ||
                value.toDouble() > std::numeric_limits<float>::max())
            {
                errorMessage =
                    QStringLiteral("placement.weights[%1] is invalid.")
                        .arg(index);
                return false;
            }
            weight = static_cast<float>(value.toDouble());
        }
        options.devices.append({ids.at(index).toString(), weight});
    }
    return inference::validateModelLoadOptions(options, errorMessage);
}

QJsonObject serializeComputeDevice(const inference::ComputeDevice& device)
{
    return {{QStringLiteral("id"), device.id},
            {QStringLiteral("backendName"), device.backendName},
            {QStringLiteral("description"), device.description},
            {QStringLiteral("hardwareId"), device.hardwareId},
            {QStringLiteral("freeMemoryBytes"),
             static_cast<qint64>(device.freeMemoryBytes)},
            {QStringLiteral("totalMemoryBytes"),
             static_cast<qint64>(device.totalMemoryBytes)}};
}

bool parseComputeDevice(const QJsonObject& object,
                        inference::ComputeDevice& device, QString& errorMessage)
{
    const auto id = object.value(QStringLiteral("id"));
    const auto backendName = object.value(QStringLiteral("backendName"));
    const auto description = object.value(QStringLiteral("description"));
    const auto hardwareId = object.value(QStringLiteral("hardwareId"));
    if (!id.isString() || id.toString().isEmpty() || !backendName.isString() ||
        backendName.toString().isEmpty() || !description.isString() ||
        !hardwareId.isString())
    {
        errorMessage = QStringLiteral("Compute device strings are invalid.");
        return false;
    }
    qint64 freeMemory = 0;
    qint64 totalMemory = 0;
    if (!readInteger(object, QStringLiteral("freeMemoryBytes"), 0,
                     std::numeric_limits<qint64>::max(), freeMemory,
                     errorMessage) ||
        !readInteger(object, QStringLiteral("totalMemoryBytes"), 0,
                     std::numeric_limits<qint64>::max(), totalMemory,
                     errorMessage) ||
        freeMemory > totalMemory)
    {
        if (freeMemory > totalMemory)
            errorMessage = QStringLiteral(
                "Compute device free memory exceeds total memory.");
        return false;
    }
    device = {id.toString(),
              backendName.toString(),
              description.toString(),
              hardwareId.toString(),
              static_cast<quint64>(freeMemory),
              static_cast<quint64>(totalMemory)};
    return true;
}

QJsonArray serializeComputeDevices(
    const QList<inference::ComputeDevice>& devices)
{
    QJsonArray result;
    for (const auto& device : devices)
        result.append(serializeComputeDevice(device));
    return result;
}

bool parseComputeDevices(const QJsonArray& array,
                         QList<inference::ComputeDevice>& devices,
                         QString& errorMessage)
{
    devices.clear();
    devices.reserve(array.size());
    for (qsizetype index = 0; index < array.size(); ++index)
    {
        if (!array.at(index).isObject())
        {
            errorMessage =
                QStringLiteral("devices[%1] must be an object.").arg(index);
            return false;
        }
        inference::ComputeDevice device;
        if (!parseComputeDevice(array.at(index).toObject(), device,
                                errorMessage))
        {
            errorMessage =
                QStringLiteral("devices[%1]: %2").arg(index).arg(errorMessage);
            return false;
        }
        devices.append(std::move(device));
    }
    return true;
}
}  // namespace qtllm::protocol
