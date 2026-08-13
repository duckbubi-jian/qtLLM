#pragma once

#include "WorkerClient.hpp"

#include <QByteArray>
#include <QMainWindow>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace qtllm::ui
{
class MainWindow final : public QMainWindow
{
    Q_OBJECT

   public:
    explicit MainWindow(QWidget* parent = nullptr);

   private slots:
    void selectModel();
    void loadSelectedModel();
    void sendPrompt();
    void stopGeneration();
    void updateState(infrastructure::WorkerClient::State state);
    void appendToken(const QByteArray& bytes);
    void finishGeneration(bool cancelled, const QJsonObject& metrics);
    void showError(const QString& code, const QString& message);

   private:
    void buildUi();
    void appendMessage(const QString& role, const QString& text);
    void flushPendingUtf8(bool final = false);

    infrastructure::WorkerClient workerClient_;
    QLineEdit* modelPathEdit_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* loadButton_ = nullptr;
    QPlainTextEdit* transcript_ = nullptr;
    QPlainTextEdit* promptEdit_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QByteArray pendingUtf8_;
};
}  // namespace qtllm::ui
