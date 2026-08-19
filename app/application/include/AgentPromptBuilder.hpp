#pragma once

#include "AssistantContext.hpp"
#include "ChatMessage.hpp"
#include "ToolDefinition.hpp"
#include "ToolResult.hpp"
#include "ToolValidation.hpp"

#include <QJsonArray>
#include <QJsonObject>
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
    [[nodiscard]] static bool requiresCompletionReview(
        const QString& userRequest);
    static chat::Message taskPlanMessage(const QString& originalRequest);
    static chat::Message taskPlanAcceptedMessage(const QJsonArray& steps);
    static chat::Message taskPlanCorrectionMessage(const QString& errorMessage);
    static chat::Message toolResultMessage(const agent::ToolResult& result,
                                           int evidenceSequence,
                                           const QJsonObject& ledgerState,
                                           const QString& verificationReason,
                                           const QString& recoveryGuidance);
    static chat::Message completionReviewMessage(
        const QString& originalRequest, const QJsonArray& completionSteps,
        const QList<QJsonObject>& toolEvidence, const QJsonObject& ledgerState,
        const QString& verificationReason);
    static chat::Message completionReviewCorrectionMessage(
        const QString& errorMessage);
    static chat::Message completionPlanDriftMessage(
        const QJsonArray& completionSteps);
    static chat::Message completionContinuationMessage(
        const QJsonArray& completionSteps, const QString& nextStep);
    static chat::Message unfinishedFinalMessage(const QString& errorMessage);
    static chat::Message correctionMessage(const QString& errorMessage);
    static chat::Message toolValidationCorrectionMessage(
        const agent::ToolDefinition& tool, const QJsonObject& arguments,
        const agent::ToolValidationIssue& issue);
    static chat::Message noProgressMessage(const QString& errorMessage);
};
}  // namespace qtllm::application
