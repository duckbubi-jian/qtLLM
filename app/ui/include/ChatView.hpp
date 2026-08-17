#pragma once

#include <QList>
#include <QString>
#include <QWidget>

#include <memory>

class QEvent;
class QMenu;
class QPlainTextEdit;
class QScrollArea;
class QVBoxLayout;

namespace Ui
{
class ChatView;
}

namespace qtllm::ui
{
enum class ModelBadgeState
{
    Neutral,
    Checking,
    Verified,
    Unverified,
    Invalid
};

struct McpServerPresentation
{
    QString serverId;
    QString displayName;
    QString detail;
    bool enabled = false;
    bool builtIn = false;
    bool available = true;
};

class ChatView final : public QWidget
{
    Q_OBJECT

   public:
    explicit ChatView(QWidget* parent = nullptr);
    ~ChatView() override;

    [[nodiscard]] QPlainTextEdit* promptEditor() const;
    [[nodiscard]] QPlainTextEdit* activityLog() const;
    [[nodiscard]] QScrollArea* conversationScroll() const;
    [[nodiscard]] QVBoxLayout* conversationLayout() const;
    [[nodiscard]] bool isAgentModeSelected() const;

    void setModelPresentation(const QString& modelPath,
                              const QString& modelInformation,
                              ModelBadgeState state = ModelBadgeState::Neutral);
    void setWorkspacePresentation(const QString& workspacePath);
    void setModelControlsEnabled(bool selectionEnabled, bool loadEnabled);
    void setWorkspaceControlsEnabled(bool enabled);
    void setPromptEnabled(bool enabled);
    void setClearEnabled(bool enabled);
    void setPrimaryAction(bool stopMode, bool enabled);
    void setAgentModeSelected(bool selected);
    void setModeSelectionEnabled(bool enabled);
    void setMcpSelectionEnabled(bool enabled);
    void setMcpServers(const QList<McpServerPresentation>& servers);
    void setConversationVisible(bool visible);
    void setStatusText(const QString& text);

   signals:
    void modelFolderRequested();
    void modelLoadRequested();
    void modelLocationRequested();
    void workspaceFolderRequested();
    void workspaceOpenRequested();
    void primaryActionRequested();
    void clearConversationRequested();
    void promptSubmitted();
    void modeChanged(bool agentMode);
    void manageMcpServersRequested();
    void addMcpServerRequested();
    void builtInFilesystemMcpToggled(bool enabled);
    void externalMcpServerToggled(const QString& serverId, bool enabled);
    void removeExternalMcpServerRequested(const QString& serverId);

   protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

   private:
    void updateModelPresentation();
    void updateWorkspacePresentation();
    void updateModePresentation();
    void rebuildMcpMenu();

    std::unique_ptr<Ui::ChatView> ui_;
    QString modelPath_;
    QString modelInformation_;
    ModelBadgeState modelBadgeState_ = ModelBadgeState::Neutral;
    QString workspacePath_;
    QList<McpServerPresentation> mcpServers_;
    QMenu* mcpMenu_ = nullptr;
    bool primaryActionStops_ = false;
};
}  // namespace qtllm::ui
