#pragma once

#include "AssistantContext.hpp"
#include "ChatMessage.hpp"
#include "ToolDefinition.hpp"
#include "ToolResult.hpp"

#include <QList>
#include <QString>

namespace qtllm::application
{
class AgentPromptBuilder final
{
   public:
    static QList<chat::Message> initialMessages(
        const QString& userRequest, const QList<agent::ToolDefinition>& tools,
        const QList<chat::Message>& conversationHistory = {},
        const AssistantContext& context = {});
    static chat::Message toolResultMessage(const agent::ToolResult& result);
    static chat::Message completionReviewMessage(
        const QString& originalRequest);
    static chat::Message correctionMessage(const QString& errorMessage);
};
}  // namespace qtllm::application
