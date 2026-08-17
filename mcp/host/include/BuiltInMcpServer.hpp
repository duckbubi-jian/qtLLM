#pragma once

#include "McpServerProcess.hpp"

#include <QString>

namespace qtllm::infrastructure::mcp
{
bool createBuiltInFilesystemServerConfig(const QString& applicationDirectory,
                                         const QString& applicationVersion,
                                         const QString& workspaceDirectory,
                                         McpServerConfig& config,
                                         QString& errorMessage);
}  // namespace qtllm::infrastructure::mcp
