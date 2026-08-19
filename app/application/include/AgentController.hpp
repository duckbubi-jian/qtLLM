#pragma once

#include "AgentAction.hpp"
#include "AgentProgress.hpp"
#include "AgentRun.hpp"
#include "AssistantContext.hpp"
#include "ModelPackage.hpp"
#include "ToolDefinition.hpp"
#include "ToolPolicy.hpp"
#include "ToolResult.hpp"
#include "ToolValidation.hpp"

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
    using ValidateToolHandler = std::function<agent::ToolValidationResult(
        const QString&, const QJsonObject&)>;
    using ToolPolicyHandler =
        std::function<infrastructure::mcp::ToolDecision(const QString&)>;
    using ToolRiskHandler =
        std::function<infrastructure::mcp::ToolRisk(const QString&)>;

    struct Dependencies
    {
        GenerateHandler generate;
        CancelGenerationHandler cancelGeneration;
        ToolCallHandler callTool;
        CancelToolHandler cancelTool;
        ValidateToolHandler validateTool;
        ToolPolicyHandler toolPolicy;
        ToolRiskHandler toolRisk;
    };

    explicit AgentController(Dependencies dependencies = {},
                             QObject* parent = nullptr);
    ~AgentController() override;

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
    [[nodiscard]] AgentProgressSnapshot progressSnapshot() const;

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
    void progressChanged(
        const qtllm::application::AgentProgressSnapshot& snapshot);
    void userRequestAccepted(const QString& runId, const QString& request);
    void approvalRequested(const QString& runId, const QString& toolName,
                           const QJsonObject& arguments);
    void finalAnswerReady(const QString& runId, const QString& content);
    void metricsReady(const QString& runId, const QJsonObject& metrics);
    void runFinished(const QString& runId,
                     qtllm::application::AgentRun::State state,
                     const QString& code, const QString& message);
    void conversationCleared();

   private:
    static bool isTerminal(AgentRun::State state);
    void requestDecision();
    void compactContextIfNeeded();
    [[nodiscard]] ExecutionTask* currentExecutionTask();
    [[nodiscard]] const ExecutionTask* currentExecutionTask() const;
    [[nodiscard]] AgentTask* currentTask();
    [[nodiscard]] const AgentTask* currentTask() const;
    [[nodiscard]] QList<chat::Message>& decisionMessages();
    [[nodiscard]] qsizetype& decisionRequestMessageIndex();
    void activateExecutionTask(int index, int evidenceStart);
    void activateSummaryTask();
    void refreshExecutionTaskSnapshots();
    void handleAction(const agent::Action& action, const QByteArray& rawAction,
                      bool recordAction = true);
    void applyTaskDirective(const AgentTask::Directive& directive);
    void acceptTaskPlan(const QJsonArray& steps, bool ordered);
    void beginCompletionReview(const agent::Action& action,
                               const QByteArray& rawAction);
    void handleCompletionReview(const agent::Action& action,
                                const QByteArray& rawAction);
    void retryCompletionReview(const QByteArray& rawAction,
                               const QString& errorMessage);
    void retryUnfinishedFinal(const QByteArray& rawAction,
                              const QString& errorMessage);
    [[nodiscard]] bool hasSufficientCompletionEvidence() const;
    [[nodiscard]] QString validateCompletionReview(
        const agent::Action& action) const;
    void executeTool(const agent::Action& action);
    void dispatchTool(const agent::Action& action);
    void retryInvalidAction(const QByteArray& rawAction,
                            const QString& errorMessage);
    void retryInvalidToolAction(const QByteArray& rawAction,
                                const agent::ToolValidationIssue& issue,
                                const agent::ToolDefinition& tool,
                                const QJsonObject& arguments);
    void retryNoProgressAction(const QByteArray& rawAction,
                               const QString& errorMessage);
    void retryOrderedPlanAction(const QByteArray& rawAction,
                                const QString& errorMessage);
    void setState(AgentRun::State state);
    void recordEvent(agent::EventType type, const QString& message = {},
                     const QString& toolName = {},
                     const QJsonObject& data = {});
    void recordTaskStarted(const AgentTask& task, int ordinal = 0,
                           int total = 0);
    void emitProgressChanged();
    void completeRun(const QString& content);
    void blockRun(const QString& reason, const QString& content);
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
