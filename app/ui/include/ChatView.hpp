#pragma once

#include <QString>
#include <QWidget>

#include <memory>

class QEvent;
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

    void setModelPresentation(const QString& modelPath,
                              const QString& modelInformation,
                              ModelBadgeState state = ModelBadgeState::Neutral);
    void setWorkspacePresentation(const QString& workspacePath);
    void setModelControlsEnabled(bool selectionEnabled, bool loadEnabled);
    void setWorkspaceControlsEnabled(bool enabled);
    void setPromptEnabled(bool enabled);
    void setClearEnabled(bool enabled);
    void setPrimaryAction(bool stopMode, bool enabled);
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

   protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

   private:
    void updateModelPresentation();
    void updateWorkspacePresentation();

    std::unique_ptr<Ui::ChatView> ui_;
    QString modelPath_;
    QString modelInformation_;
    ModelBadgeState modelBadgeState_ = ModelBadgeState::Neutral;
    QString workspacePath_;
    bool primaryActionStops_ = false;
};
}  // namespace qtllm::ui
