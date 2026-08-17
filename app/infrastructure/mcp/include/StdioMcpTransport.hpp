#pragma once

#include "McpServerProcess.hpp"
#include "McpTransport.hpp"

#include <QHash>
#include <QTimer>

namespace qtllm::infrastructure::mcp
{
class StdioMcpTransport final : public McpTransport
{
    Q_OBJECT

   public:
    explicit StdioMcpTransport(McpServerConfig config,
                               QObject* parent = nullptr);
    ~StdioMcpTransport() override;

    [[nodiscard]] const McpServerConfig& config() const;
    [[nodiscard]] bool isRunning() const override;

   public slots:
    void start() override;
    void stop() override;
    QString request(const QString& method, const QJsonObject& params,
                    int timeoutMs = 30'000) override;
    void cancel(const QString& requestId) override;
    void cancelAll() override;

    bool notify(const QString& method, const QJsonObject& params = {}) override;
    bool respond(const QJsonValue& requestId,
                 const QJsonObject& result) override;
    bool respondError(const QJsonValue& requestId, int code,
                      const QString& message) override;

   private slots:
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error, const QString& message);

   private:
    struct PendingRequest
    {
        QString method;
        QTimer* timer = nullptr;
    };

    QString writeRequest(const QJsonObject& object, const QString& method,
                         int timeoutMs);
    bool writeMessage(const QJsonObject& object);
    void processLine(const QByteArray& line);
    void failPending(const QString& requestId, const QString& code,
                     const QString& message);

    McpServerProcess process_;
    QByteArray standardOutputBuffer_;
    QHash<QString, PendingRequest> pending_;
    bool stopping_ = false;
};
}  // namespace qtllm::infrastructure::mcp
