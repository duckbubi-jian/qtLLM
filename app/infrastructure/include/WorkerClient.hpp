#pragma once

#include "ChatMessage.hpp"
#include "ComputeDevice.hpp"
#include "InferenceDefaults.hpp"
#include "JsonLineProtocol.hpp"
#include "ResponseMode.hpp"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

namespace qtllm::infrastructure
{
class WorkerClient final : public QObject
{
    Q_OBJECT

   public:
    struct Capabilities
    {
        bool structuredGeneration = false;
        bool grammar = false;
        bool gpuDeviceDiscovery = false;
        bool multiGpuLayerSplit = false;
    };

    enum class State
    {
        Stopped,
        Starting,
        Ready,
        UnloadingModel,
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
    void loadModel(const QString& modelPath,
                   const inference::ModelLoadOptions& options = {});
    void unloadModel();
    void refreshComputeDevices();
    void generate(
        const QList<chat::Message>& messages,
        int contextSize = inference::defaultContextSize,
        int maxTokens = inference::defaultMaxOutputTokens, int threads = 0,
        float temperature = 0.6F, float topP = 0.95F, int topK = 40,
        float repeatPenalty = 1.05F,
        inference::ResponseMode responseMode = inference::ResponseMode::Text,
        const QString& grammar = {});
    void generate(
        const QString& prompt, const QString& systemPrompt,
        int contextSize = inference::defaultContextSize,
        int maxTokens = inference::defaultMaxOutputTokens, int threads = 0,
        float temperature = 0.6F, float topP = 0.95F, int topK = 40,
        float repeatPenalty = 1.05F,
        inference::ResponseMode responseMode = inference::ResponseMode::Text,
        const QString& grammar = {});
    void cancel();

    [[nodiscard]] State state() const;
    [[nodiscard]] QString modelPath() const;
    [[nodiscard]] QString activeGenerationRequestId() const;
    [[nodiscard]] Capabilities capabilities() const;
    [[nodiscard]] QList<inference::ComputeDevice> computeDevices() const;
    [[nodiscard]] QList<inference::ComputeDevice> activeComputeDevices() const;
    [[nodiscard]] QString activeSplitMode() const;

   signals:
    void stateChanged(qtllm::infrastructure::WorkerClient::State state);
    void modelLoaded(const QString& modelPath, qint64 loadMilliseconds,
                     const QString& device);
    void modelUnloaded();
    void modelLoadFailed(const QString& code, const QString& message);
    void modelUnloadFailed(const QString& code, const QString& message);
    void tokenReceived(const QByteArray& bytes);
    void generationProgress(int generatedTokens, double tokensPerSecond,
                            qint64 contextMilliseconds);
    void generationFinished(bool cancelled, const QJsonObject& metrics);
    void errorOccurred(const QString& code, const QString& message);
    void diagnosticReceived(const QString& text);
    void computeDevicesChanged(
        const QList<qtllm::inference::ComputeDevice>& devices);

   private slots:
    void onStarted();
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

   private:
    QString send(const QString& type, const QJsonObject& payload = {});
    void handleMessage(const protocol::Message& message);
    void emitGenerationProgress();
    void setState(State state);
    void fail(const QString& code, const QString& message);
    [[nodiscard]] QString defaultWorkerPath() const;

    QProcess process_;
    QByteArray standardOutputBuffer_;
    State state_ = State::Stopped;
    QString helloRequestId_;
    QString loadRequestId_;
    QString unloadRequestId_;
    QString devicesRequestId_;
    QString generationRequestId_;
    QString modelPath_;
    QString activeSplitMode_ = QStringLiteral("cpu");
    QList<inference::ComputeDevice> computeDevices_;
    QList<inference::ComputeDevice> activeComputeDevices_;
    Capabilities capabilities_;
    QElapsedTimer generationRequestTimer_;
    QElapsedTimer tokenGenerationTimer_;
    QTimer generationProgressTimer_;
    int liveGeneratedTokens_ = 0;
    qint64 firstTokenMilliseconds_ = -1;
    bool stopping_ = false;
};
}  // namespace qtllm::infrastructure
