#pragma once

#include "ToolResult.hpp"

namespace qtllm::application
{
[[nodiscard]] agent::ToolOutcome normalizedToolOutcome(
    const agent::ToolResult& result);
[[nodiscard]] agent::ToolSideEffectState normalizedToolSideEffectState(
    const agent::ToolResult& result);
[[nodiscard]] QString toolOutcomeName(agent::ToolOutcome outcome);
[[nodiscard]] QString toolSideEffectStateName(agent::ToolSideEffectState state);
[[nodiscard]] bool toolResultIndicatesInProgress(
    const agent::ToolResult& result);
}  // namespace qtllm::application
