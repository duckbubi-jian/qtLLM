#pragma once

#include "AgentController.hpp"
#include "ChatController.hpp"
#include "McpClientManager.hpp"
#include "ModelPackage.hpp"
#include "SettingsStore.hpp"
#include "WorkerClient.hpp"

#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QSet>
#include <QString>

class QTimer;

namespace qtllm::ui
{
class ChatView;
class MessageWidget;
class McpControlPanel;
class ToolApprovalWidget;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

   public:
    explicit MainWindow(QWidget* parent = nullptr);
    explicit MainWindow(const QString& settingsFilePath,
                        QWidget* parent = nullptr);

   private slots:
    void selectModelPackage();
    void openModelDirectory();
    void showComputeSettings();
    void selectWorkspaceDirectory();
    void openWorkspaceDirectory();
    void addMcpServer();
    void setBuiltInFilesystemMcpEnabled(bool enabled);
    void setExternalMcpServerEnabled(const QString& serverId, bool enabled);
    void setMcpInstructionsEnabled(const QString& serverId, bool builtIn,
                                   bool enabled);
    void setMcpLoggingLevel(const QString& serverId, const QString& level);
    void removeExternalMcpServer(const QString& serverId);
    void showMcpControlPanel();
    void startMcpServer(const QString& serverId);
    void stopMcpServer(const QString& serverId);
    void restartMcpServer(const QString& serverId);
    void pingMcpServer(const QString& serverId);
    void refreshMcpTools(const QString& serverId);
    void readMcpResource(const QString& serverId, const QString& uri);
    void setMcpResourceSubscribed(const QString& serverId, const QString& uri,
                                  bool subscribe);
    void getMcpPrompt(const QString& serverId, const QString& name,
                      const QJsonObject& arguments);
    void completeMcpPrompt(const QString& serverId, const QString& name,
                           const QString& argumentName, const QString& value,
                           const QJsonObject& contextArguments);
    void completeMcpResourceTemplate(const QString& serverId,
                                     const QString& uriTemplate,
                                     const QString& argumentName,
                                     const QString& value);
    void loadSelectedModel();
    void finishModelPackageVerification();
    void continueModelLoadAfterUnload();
    void handleModelLoadFailure(const QString& code, const QString& message);
    void handleModelUnloadFailure(const QString& code, const QString& message);
    void triggerPrimaryAction();
    void sendPrompt();
    void stopGeneration();
    void clearConversation();
    void updateState(infrastructure::WorkerClient::State state);
    void appendToken(const QByteArray& bytes);
    void finishGeneration(bool cancelled, const QJsonObject& metrics);
    void showError(const QString& code, const QString& message);
    void advanceThinkingAnimation();

   private:
    void buildUi();
    void appendUserMessage(const QString& text);
    void beginAssistantMessage();
    void appendActivityText(const QString& text);
    void appendAgentEvent(const agent::Event& event);
    void renderAssistant(bool final = false);
    [[nodiscard]] bool conversationIsAtBottom() const;
    void scrollConversationToBottom();
    void flushPendingUtf8(bool final = false);
    void resetConversationView();
    void updateClearButton();
    void beginModelLoad(models::ModelSelection selection);
    void startPendingModelLoad();
    [[nodiscard]] bool modelHashesAreCached(
        const models::ModelSelection& selection) const;
    bool cacheVerifiedModel(const models::ModelSelection& selection) const;
    void updateModelInformation(const models::ModelSelection& selection);
    void updateModelVerificationProgress(quint64 verifiedBytes,
                                         quint64 totalBytes);
    void beginChatPrompt(const QString& prompt);
    void beginAgentPrompt(const QString& prompt);
    void showAgentActivity();
    void startThinkingAnimation();
    void stopThinkingAnimation();
    void updateAgentActivity(const QString& text);
    void removeAgentActivity();
    void appendAgentAnswer(const QString& answer);
    void updateAgentState(application::AgentRun::State state);
    void loadMcpServers();
    void refreshMcpServerMenu();
    void refreshMcpControlPanel();
    void appendMcpDiagnostic(const QString& serverId, const QString& category,
                             const QString& text);
    void setModelPath(const QString& modelPath);
    void updateComputePresentation();
    void updatePrimaryAction(bool stopMode);
    bool startBuiltInFilesystem(const QString& workspacePath,
                                QString& errorMessage);

    infrastructure::SettingsStore settingsStore_;
    infrastructure::WorkerClient workerClient_;
    infrastructure::mcp::McpClientManager mcpManager_;
    infrastructure::mcp::ToolPolicy toolPolicy_;
    application::ChatController chatController_;
    application::AgentController agentController_;
    QFutureWatcher<models::ModelPackageResult> modelVerificationWatcher_;
    ChatView* chatView_ = nullptr;
    QTimer* renderTimer_ = nullptr;
    QTimer* thinkingAnimationTimer_ = nullptr;
    MessageWidget* currentAssistant_ = nullptr;
    MessageWidget* agentActivityMessage_ = nullptr;
    ToolApprovalWidget* pendingToolApproval_ = nullptr;
    McpControlPanel* mcpControlPanel_ = nullptr;
    QString currentAssistantText_;
    QString modelPath_;
    QString modelInfoText_;
    QString workspacePath_;
    QByteArray pendingUtf8_;
    QList<chat::Message> conversationMessages_;
    QList<infrastructure::mcp::McpServerConfig> mcpConfigurations_;
    QHash<QString, QString> mcpServerErrors_;
    QHash<QString, QStringList> mcpDiagnostics_;
    QHash<QString, QString> pendingMcpLoggingLevels_;
    QSet<QString> pendingMcpRestarts_;
    models::ModelSelection pendingModelSelection_;
    models::ModelSelection activeModelSelection_;
    inference::ModelLoadOptions modelLoadOptions_;
    bool verifyingModelPackage_ = false;
    int modelVerificationPercent_ = -1;
    bool replacingModel_ = false;
    bool agentRunActive_ = false;
    bool primaryActionStops_ = false;
    bool builtInFilesystemRunning_ = false;
    bool filesystemConfiguredExternally_ = false;
    int thinkingAnimationFrame_ = 1;
};
}  // namespace qtllm::ui
