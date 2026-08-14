#pragma once

#include <QString>
#include <QStringView>

namespace qtllm::logging
{
bool initialize(QStringView component, QString* errorMessage = nullptr);
void installQtMessageHandler();
void shutdown();

[[nodiscard]] QString logDirectory();
[[nodiscard]] QString logFilePath();

void debug(QStringView message);
void info(QStringView message);
void warning(QStringView message);
void error(QStringView message);
void critical(QStringView message);
}  // namespace qtllm::logging
