#pragma once

#include <QJsonValue>

namespace qtllm::ui
{
[[nodiscard]] QJsonValue redactSensitiveValues(const QJsonValue& value);
}  // namespace qtllm::ui
