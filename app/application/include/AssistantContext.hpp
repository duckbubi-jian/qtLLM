#pragma once

#include <QString>

namespace qtllm::application
{
struct AssistantContext
{
    QString modelName;
    QString workspaceRoot;
};

[[nodiscard]] QString assistantContextJson(const AssistantContext& context);
}  // namespace qtllm::application
