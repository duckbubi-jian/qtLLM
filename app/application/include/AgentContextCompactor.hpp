#pragma once

#include "AgentAction.hpp"
#include "ChatMessage.hpp"
#include "ModelPackage.hpp"
#include "ToolResult.hpp"

#include <QJsonObject>
#include <QList>

namespace qtllm::application
{
class AgentContextCompactor final
{
   public:
    struct Result
    {
        bool compacted = false;
        qsizetype messagesBefore = 0;
        qsizetype messagesAfter = 0;
    };

    [[nodiscard]] static bool shouldCompact(
        int promptTokens, const models::InferencePreset& preset);
    [[nodiscard]] static QJsonObject toolEvidence(
        int sequence, const agent::Action& action,
        const agent::ToolResult& result);
    [[nodiscard]] static Result compact(QList<chat::Message>& messages,
                                        qsizetype& requestMessageIndex,
                                        const QString& originalRequest,
                                        const QList<QJsonObject>& toolEvidence,
                                        const QJsonObject& completionState,
                                        const models::InferencePreset& preset);
};
}  // namespace qtllm::application
