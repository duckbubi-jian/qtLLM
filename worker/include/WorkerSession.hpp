#pragma once

#include "LlamaEngine.hpp"

#include "JsonLineProtocol.hpp"

#include <QByteArray>
#include <QFile>
#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>
#include <thread>

namespace qtllm::worker
{
class WorkerSession final : public QObject
{
    Q_OBJECT

   public:
    explicit WorkerSession(QObject* parent = nullptr);
    ~WorkerSession() override;

    bool start(QString& errorMessage);

   public slots:
    void processLine(const QByteArray& line);

   private:
    void dispatch(const protocol::Message& message);
    void handleHello(const protocol::Message& message);
    void handleLoadModel(const protocol::Message& message);
    void handleUnloadModel(const protocol::Message& message);
    void handleGenerate(const protocol::Message& message);
    void handleCancel(const protocol::Message& message);
    void handleGetStatus(const protocol::Message& message);
    void finishGeneration(const QString& requestId, bool generated,
                          const QString& errorMessage,
                          const GenerationMetrics& metrics);
    void send(const protocol::Message& message);
    void sendError(const QString& requestId, const QString& code,
                   const QString& message);
    void sendStatus(const QString& requestId,
                    const QJsonObject& additionalPayload = {});

    LlamaEngine engine_;
    QFile output_;
    QMutex outputMutex_;
    std::thread generationThread_;
    std::atomic_bool cancelRequested_ = false;
    bool generating_ = false;
    QString generationRequestId_;
};
}  // namespace qtllm::worker
