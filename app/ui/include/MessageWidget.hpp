#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QResizeEvent;
class QTextBrowser;
class QToolButton;
class QVBoxLayout;

namespace qtllm::ui
{
class MessageWidget final : public QWidget
{
    Q_OBJECT

   public:
    enum class Role
    {
        User,
        Assistant
    };

    explicit MessageWidget(Role role, QWidget* parent = nullptr);

    void setUserText(const QString& text);
    void setAssistantText(const QString& rawText, bool final);

   protected:
    void resizeEvent(QResizeEvent* event) override;

   private:
    void updateUserBubbleWidth();

    Role role_;
    QLabel* roleLabel_ = nullptr;
    QToolButton* reasoningToggle_ = nullptr;
    QTextBrowser* reasoningView_ = nullptr;
    QTextBrowser* bodyView_ = nullptr;
    QWidget* codeActions_ = nullptr;
    QVBoxLayout* codeActionsLayout_ = nullptr;
    bool hadReasoning_ = false;
};
}  // namespace qtllm::ui
