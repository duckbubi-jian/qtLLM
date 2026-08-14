#pragma once

#include "ChatMessage.hpp"
#include "ModelPackage.hpp"
#include "SettingsStore.hpp"
#include "WorkerClient.hpp"

#include <QByteArray>
#include <QFutureWatcher>
#include <QList>
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

class MainWindow final : public QMainWindow
{
    Q_OBJECT

   public:
    explicit MainWindow(QWidget* parent = nullptr);

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
    void showError(const QString& code, const QString& message);

   private:
    void buildUi();
    void appendUserMessage(const QString& text);
    void beginAssistantMessage();
    void appendRawText(const QString& text);
    void renderAssistant(bool final = false);
    [[nodiscard]] bool conversationIsAtBottom() const;
    void scrollConversationToBottom();
    void flushPendingUtf8(bool final = false);
    void discardPendingHistoryMessage();
    void updateClearButton();
    void beginModelLoad(models::ModelSelection selection);
    [[nodiscard]] bool modelHashesAreCached(
        const models::ModelSelection& selection) const;
    bool cacheVerifiedModel(const models::ModelSelection& selection) const;
    void updateModelInformation(const models::ModelSelection& selection);

    infrastructure::SettingsStore settingsStore_;
    infrastructure::WorkerClient workerClient_;
    QFutureWatcher<models::ModelPackageResult> modelVerificationWatcher_;
    QLineEdit* modelPathEdit_ = nullptr;
    QLabel* modelInfoLabel_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* loadButton_ = nullptr;
    QTabWidget* transcriptTabs_ = nullptr;
    QScrollArea* conversationScroll_ = nullptr;
    QVBoxLayout* conversationLayout_ = nullptr;
    QPlainTextEdit* rawTranscript_ = nullptr;
    QPlainTextEdit* promptEdit_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QTimer* renderTimer_ = nullptr;
    MessageWidget* currentAssistant_ = nullptr;
    QList<chat::Message> conversationMessages_;
    QString currentAssistantText_;
    QByteArray pendingUtf8_;
    models::ModelSelection pendingModelSelection_;
    models::ModelSelection activeModelSelection_;
    bool hasPendingHistoryMessage_ = false;
    bool verifyingModelPackage_ = false;
};
}  // namespace qtllm::ui
