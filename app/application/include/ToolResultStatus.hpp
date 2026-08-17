#pragma once

#include "ToolResult.hpp"

namespace qtllm::application
{
[[nodiscard]] bool toolResultIndicatesInProgress(
    const agent::ToolResult& result);
}  // namespace qtllm::application
