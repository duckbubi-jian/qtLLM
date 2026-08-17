#pragma once

#include "AgentAction.hpp"
#include "AgentRun.hpp"
#include "AssistantContext.hpp"
#include "ModelPackage.hpp"
#include "ToolDefinition.hpp"
#include "ToolPolicy.hpp"
#include "ToolResult.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <optional>

namespace qtllm::application
{
class AgentController final : public QObject
{
    Q_OBJECT

   public:
    using GenerateHandler = std::function<void(
        const QList<chat::Message>&, const models::InferencePreset&, int)>;
    using CancelGenerationHandler = std::function<void()>;
    using ToolCallHandler =
        std::function<QString(const QString&, const QJsonObject&)>;
    using CancelToolHandler = std::function<void(const QString&)>;
    using ValidateToolHandler =
        std::function<bool(const QString&, const QJsonObject&, QString&)>;
    using ToolPolicyHandler =
        std::function<infrastructure::mcp::ToolDecision(const QString&)>;

    struct Dependencies
    {
        GenerateHandler generate;
        CancelGenerationHandler cancelGeneration;
        ToolCallHandler callTool;
        CancelToolHandler cancelTool;
        ValidateToolHandler validateTool;
        ToolPolicyHandler toolPolicy;
    };

    explicit AgentController(Dependencies dependencies = {},
                             QObject* parent = nullptr);

    bool start(const QString& userRequest,
               const models::InferencePreset& preset,
               const QList<agent::ToolDefinition>& tools,
               const AssistantContext& context = {});
    void cancel();
    void resolveApproval(bool approved);
    bool clearConversation();
    bool setConversationMessages(QList<chat::Message> messages);

    [[nodiscard]] AgentRun::State state() const;
    [[nodiscard]] bool hasActiveRun() const;
    [[nodiscard]] const std::optional<AgentRun>& activeRun() const;
    [[nodiscard]] const QList<chat::Message>& conversationMessages() const;
    [[nodiscard]] bool hasConversation() const;

   public slots:
    void receiveToken(const QByteArray& bytes);
    void completeGeneration(bool cancelled, const QJsonObject& metrics = {});
    void handleGenerationError(const QString& code, const QString& message);
    void receiveToolResult(const qtllm::agent::ToolResult& result);

   private slots:
    void notifyLongRunning();
    void executePendingPoll();

   signals:
    void stateChanged(qtllm::application::AgentRun::State state);
    void eventRecorded(const qtllm::agent::Event& event);
    void userRequestAccepted(const QString& runId, const QString& request);
    void approvalRequested(const QString& runId, const QString& toolName,
                           const QJsonObject& arguments);
    void finalAnswerReady(const QString& runId, const QString& content);
    void runFinished(const QString& runId,
                     qtllm::application::AgentRun::State state,
                     const QString& code, const QString& message);
    void conversationCleared();

   private:
    static bool isTerminal(AgentRun::State state);
    void requestDecision();
    void compactContextIfNeeded();
    void handleAction(const agent::Action& action, const QByteArray& rawAction);
    void acceptTaskPlan(const agent::Action& action,
                        const QByteArray& rawAction);
    void beginCompletionReview(const agent::Action& action,
                               const QByteArray& rawAction);
    void handleCompletionReview(const agent::Action& action,
                                const QByteArray& rawAction);
    void retryTaskPlan(const QByteArray& rawAction,
                       const QString& errorMessage);
    void retryCompletionReview(const QByteArray& rawAction,
                               const QString& errorMessage);
    void retryUnfinishedFinal(const QByteArray& rawAction,
                              const QString& errorMessage);
    [[nodiscard]] bool hasSufficientCompletionEvidence() const;
    [[nodiscard]] QString validateCompletionReview(
        const agent::Action& action) const;
    void executeTool(const agent::Action& action);
    void retryInvalidAction(const QByteArray& rawAction,
                            const QString& errorMessage);
    void retryNoProgressAction(const QByteArray& rawAction,
                               const QString& errorMessage);
    void setState(AgentRun::State state);
    void recordEvent(agent::EventType type, const QString& message = {},
                     const QString& toolName = {},
                     const QJsonObject& data = {});
    void completeRun(const QString& content);
    void failRun(const QString& code, const QString& message);

    Dependencies dependencies_;
    AgentRun::State state_ = AgentRun::State::Idle;
    std::optional<AgentRun> activeRun_;
    models::InferencePreset preset_;
    QList<agent::ToolDefinition> availableTools_;
    QList<chat::Message> conversationMessages_;
    std::optional<agent::Action> pendingApproval_;
    std::optional<agent::Action> activeToolAction_;
    std::optional<agent::Action> pendingPollAction_;
    QByteArray decisionBytes_;
    QString activeToolCallSignature_;
    QString lastFailedToolCallSignature_;
    QString pollableToolCallSignature_;
    qint64 lastPollCompletedAtMs_ = 0;
    QStringList completedToolCallHistory_;
    QList<QJsonObject> toolEvidence_;
    QTimer* runTimer_ = nullptr;
    QTimer* pollTimer_ = nullptr;
};
}  // namespace qtllm::application
