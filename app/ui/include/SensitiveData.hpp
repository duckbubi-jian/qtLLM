#pragma once

#include <QJsonValue>
#include <QString>

namespace qtllm::ui
{
[[nodiscard]] QJsonValue redactSensitiveValues(const QJsonValue& value);
[[nodiscard]] QString redactSensitiveText(const QString& text,
                                          qsizetype maximumCharacters = 4'096);
}  // namespace qtllm::ui
