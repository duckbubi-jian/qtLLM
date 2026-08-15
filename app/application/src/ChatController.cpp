#include "ChatController.hpp"

#include "AssistantResponse.hpp"

#include <QJsonDocument>

#include <utility>

namespace qtllm::application
{
namespace
{
QString systemPrompt(const AssistantContext& context)
{
    auto prompt = QStringLiteral(
        "You are a helpful local assistant. Answer in the user's language.");
    const auto json = assistantContextJson(context);
    if (!json.isEmpty())
        prompt += QStringLiteral(
                      " Runtime context: %1. Use these exact values for "
                      "model and workspace questions.")
                      .arg(json);
    return prompt;
}
}  // namespace

ChatController::ChatController(GenerateHandler generateHandler,
                               CancelHandler cancelHandler, QObject* parent)
    : QObject(parent),
      generateHandler_(std::move(generateHandler)),
      cancelHandler_(std::move(cancelHandler))
{
}

bool ChatController::sendPrompt(const QString& prompt,
                                const models::InferencePreset& preset,
                                const AssistantContext& context)
{
    const auto trimmedPrompt = prompt.trimmed();
    if (trimmedPrompt.isEmpty() || generationPending_ || !generateHandler_)
        return false;

    conversationMessages_.append({chat::Role::User, trimmedPrompt});
    responseBytes_.clear();
    generationPending_ = true;
    emit userMessageAccepted(trimmedPrompt);
    emit assistantResponseStarted();

    auto requestMessages = conversationMessages_;
    requestMessages.prepend({chat::Role::System, systemPrompt(context)});
    generateHandler_(requestMessages, preset);
    return true;
}

void ChatController::cancel()
{
    if (generationPending_ && cancelHandler_) cancelHandler_();
}

bool ChatController::clearConversation()
{
    if (generationPending_) return false;
    conversationMessages_.clear();
    responseBytes_.clear();
    emit conversationCleared();
    return true;
}

bool ChatController::setConversationMessages(QList<chat::Message> messages)
{
    if (generationPending_) return false;
    conversationMessages_ = std::move(messages);
    return true;
}

const QList<chat::Message>& ChatController::conversationMessages() const
{
    return conversationMessages_;
}

bool ChatController::hasConversation() const
{
    return !conversationMessages_.isEmpty();
}

bool ChatController::isGenerating() const
{
    return generationPending_;
}

void ChatController::receiveToken(const QByteArray& bytes)
{
    if (!generationPending_ || bytes.isEmpty()) return;
    responseBytes_ += bytes;
    emit tokenReceived(bytes);
}

void ChatController::completeGeneration(bool cancelled,
                                        const QJsonObject& metrics)
{
    if (!generationPending_) return;

    if (!cancelled)
    {
        const auto answer =
            chat::assistantHistoryText(QString::fromUtf8(responseBytes_));
        if (!answer.isEmpty())
            conversationMessages_.append({chat::Role::Assistant, answer});
        else
            discardPendingUserMessage();
    }
    else
    {
        discardPendingUserMessage();
    }

    generationPending_ = false;
    responseBytes_.clear();
    emit generationFinished(cancelled, metrics);
}

void ChatController::handleError(const QString& code, const QString& message)
{
    QString retryPrompt;
    if (generationPending_ && !conversationMessages_.isEmpty() &&
        conversationMessages_.constLast().role == chat::Role::User)
    {
        retryPrompt = conversationMessages_.constLast().content;
        discardPendingUserMessage();
    }

    generationPending_ = false;
    responseBytes_.clear();
    emit errorOccurred(code, message, retryPrompt);
}

void ChatController::discardPendingUserMessage()
{
    if (!conversationMessages_.isEmpty() &&
        conversationMessages_.constLast().role == chat::Role::User)
    {
        conversationMessages_.removeLast();
    }
}
}  // namespace qtllm::application
