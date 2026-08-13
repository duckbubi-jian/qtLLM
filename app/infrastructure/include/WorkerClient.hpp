#pragma once

#include "ChatMessage.hpp"
#include "JsonLineProtocol.hpp"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>

namespace qtllm::infrastructure
{
class WorkerClient final : public QObject
{
    Q_OBJECT

   public:
    enum class State
    {
        Stopped,
        Starting,
        Ready,
        LoadingModel,
        ModelReady,
        Generating,
        Failed
    };
    Q_ENUM(State)

    explicit WorkerClient(QObject* parent = nullptr);
    ~WorkerClient() override;

    void start(const QString& workerPath = {});
    void stop();
    void loadModel(const QString& modelPath, int gpuLayers = -1);
    void unloadModel();
    void generate(const QList<chat::Message>& messages, int contextSize = 8192,
                  int maxTokens = 1024, int threads = 0,
                  float temperature = 0.6F, float topP = 0.95F, int topK = 40,
                  float repeatPenalty = 1.05F);
    void generate(const QString& prompt, const QString& systemPrompt,
                  int contextSize = 8192, int maxTokens = 1024, int threads = 0,
                  float temperature = 0.6F, float topP = 0.95F, int topK = 40,
                  float repeatPenalty = 1.05F);
    void cancel();

    [[nodiscard]] State state() const;
    [[nodiscard]] QString modelPath() const;
    [[nodiscard]] QString activeGenerationRequestId() const;

   signals:
    void stateChanged(qtllm::infrastructure::WorkerClient::State state);
    void modelLoaded(const QString& modelPath, qint64 loadMilliseconds,
                     const QString& device);
    void tokenReceived(const QByteArray& bytes);
    void generationFinished(bool cancelled, const QJsonObject& metrics);
    void errorOccurred(const QString& code, const QString& message);
    void diagnosticReceived(const QString& text);

   private slots:
    void onStarted();
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

   private:
    QString send(const QString& type, const QJsonObject& payload = {});
    void handleMessage(const protocol::Message& message);
    void setState(State state);
    void fail(const QString& code, const QString& message);
    [[nodiscard]] QString defaultWorkerPath() const;

    QProcess process_;
    QByteArray standardOutputBuffer_;
    State state_ = State::Stopped;
    QString helloRequestId_;
    QString loadRequestId_;
    QString unloadRequestId_;
    QString generationRequestId_;
    QString modelPath_;
    bool stopping_ = false;
};
}  // namespace qtllm::infrastructure
