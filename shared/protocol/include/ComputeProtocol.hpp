#pragma once

#include "ComputeDevice.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace qtllm::protocol
{
[[nodiscard]] QJsonObject serializeModelLoadOptions(
    const inference::ModelLoadOptions& options);
bool parseModelLoadOptions(const QJsonObject& object,
                           inference::ModelLoadOptions& options,
                           QString& errorMessage);

[[nodiscard]] QJsonObject serializeComputeDevice(
    const inference::ComputeDevice& device);
bool parseComputeDevice(const QJsonObject& object,
                        inference::ComputeDevice& device,
                        QString& errorMessage);
[[nodiscard]] QJsonArray serializeComputeDevices(
    const QList<inference::ComputeDevice>& devices);
bool parseComputeDevices(const QJsonArray& array,
                         QList<inference::ComputeDevice>& devices,
                         QString& errorMessage);
}  // namespace qtllm::protocol
