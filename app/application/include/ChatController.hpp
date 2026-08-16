#pragma once

#include "AssistantContext.hpp"
#include "ChatMessage.hpp"
#include "ModelPackage.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

namespace qtllm::application
{
class ChatController final : public QObject
{
    Q_OBJECT

   public:
    using GenerateHandler = std::function<void(const QList<chat::Message>&,
                                               const models::InferencePreset&)>;
    using CancelHandler = std::function<void()>;

    explicit ChatController(GenerateHandler generateHandler,
                            CancelHandler cancelHandler,
                            QObject* parent = nullptr);

    bool sendPrompt(const QString& prompt,
                    const models::InferencePreset& preset,
                    const AssistantContext& context = {});
    void cancel();
    bool clearConversation();
    bool setConversationMessages(QList<chat::Message> messages);

    [[nodiscard]] const QList<chat::Message>& conversationMessages() const;
    [[nodiscard]] bool hasConversation() const;
    [[nodiscard]] bool isGenerating() const;

   public slots:
    void receiveToken(const QByteArray& bytes);
    void completeGeneration(bool cancelled, const QJsonObject& metrics);
    void handleError(const QString& code, const QString& message);

   signals:
    void userMessageAccepted(const QString& prompt);
    void assistantResponseStarted();
    void tokenReceived(const QByteArray& bytes);
    void generationFinished(bool cancelled, const QJsonObject& metrics);
    void errorOccurred(const QString& code, const QString& message);
    void conversationCleared();

   private:
    void discardPendingUserMessage();

    GenerateHandler generateHandler_;
    CancelHandler cancelHandler_;
    QList<chat::Message> conversationMessages_;
    QByteArray responseBytes_;
    bool generationPending_ = false;
};
}  // namespace qtllm::application
