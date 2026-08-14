#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace qtllm::infrastructure::mcp
{
class McpTransport : public QObject
{
    Q_OBJECT

   public:
    using QObject::QObject;
    ~McpTransport() override = default;

   public slots:
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual QString request(const QString& method, const QJsonObject& params,
                            int timeoutMs = 30'000) = 0;
    virtual void cancel(const QString& requestId) = 0;
    virtual void cancelAll() = 0;

   signals:
    void responseReceived(const QString& requestId, const QString& method,
                          const QJsonObject& result);
    void requestFailed(const QString& requestId, const QString& method,
                       const QString& code, const QString& message);
    void notificationReceived(const QString& method, const QJsonObject& params);
    void diagnosticReceived(const QString& text);
    void started();
    void stopped();
    void transportError(const QString& code, const QString& message);
};
}  // namespace qtllm::infrastructure::mcp
