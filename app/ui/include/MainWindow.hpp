#pragma once

#include "AgentController.hpp"
#include "McpClientManager.hpp"
#include "ModelPackage.hpp"
#include "SettingsStore.hpp"
#include "WorkerClient.hpp"

#include <QByteArray>
#include <QFutureWatcher>
#include <QMainWindow>
#include <QString>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTabWidget;
class QTimer;
class QVBoxLayout;

namespace qtllm::ui
{
class MessageWidget;
class ToolApprovalWidget;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

   public:
    explicit MainWindow(QWidget* parent = nullptr);

   protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

   private slots:
    void selectModelPackage();
    void selectGgufModel();
    void loadSelectedModel();
    void finishModelPackageVerification();
    void sendPrompt();
    void stopGeneration();
    void clearConversation();
    void updateState(infrastructure::WorkerClient::State state);
    void appendToken(const QByteArray& bytes);
    void finishGeneration(bool cancelled, const QJsonObject& metrics);
    void showError(const QString& code, const QString& message,
                   const QString& retryPrompt = {});

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
    [[nodiscard]] bool modelHashesAreCached(
        const models::ModelSelection& selection) const;
    bool cacheVerifiedModel(const models::ModelSelection& selection) const;
    void updateModelInformation(const models::ModelSelection& selection);
    void beginAgentPrompt(const QString& prompt);
    void showAgentActivity();
    void updateAgentActivity(const QString& text);
    void removeAgentActivity();
    void appendAgentAnswer(const QString& answer);
    void updateAgentState(application::AgentRun::State state);
    void loadMcpServers();

    infrastructure::SettingsStore settingsStore_;
    infrastructure::WorkerClient workerClient_;
    infrastructure::mcp::McpClientManager mcpManager_;
    infrastructure::mcp::ToolPolicy toolPolicy_;
    application::AgentController agentController_;
    QFutureWatcher<models::ModelPackageResult> modelVerificationWatcher_;
    QLineEdit* modelPathEdit_ = nullptr;
    QLabel* modelInfoLabel_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* loadButton_ = nullptr;
    QTabWidget* transcriptTabs_ = nullptr;
    QScrollArea* conversationScroll_ = nullptr;
    QVBoxLayout* conversationLayout_ = nullptr;
    QPlainTextEdit* activityLog_ = nullptr;
    QPlainTextEdit* promptEdit_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QTimer* renderTimer_ = nullptr;
    MessageWidget* currentAssistant_ = nullptr;
    MessageWidget* agentActivityMessage_ = nullptr;
    ToolApprovalWidget* pendingToolApproval_ = nullptr;
    QString currentAssistantText_;
    QByteArray pendingUtf8_;
    models::ModelSelection pendingModelSelection_;
    models::ModelSelection activeModelSelection_;
    bool verifyingModelPackage_ = false;
    bool agentRunActive_ = false;
};
}  // namespace qtllm::ui
