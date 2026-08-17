#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QStringList>

namespace qtllm::infrastructure::mcp
{
struct McpServerConfig
{
    QString serverId;
    QString program;
    QStringList arguments;
    QMap<QString, QString> environment;
    QString workingDirectory;
    QStringList toolAllowlist;
    QStringList authorizedRoots;
    QString loggingLevel;
    bool enabled = true;
    bool useInstructions = true;
    int initializeTimeoutMs = 10'000;
    int requestTimeoutMs = 30'000;
    qsizetype maxResultBytes = 65'536;
};

QJsonObject serializeServerConfig(const McpServerConfig& config);
bool parseServerConfig(const QJsonObject& object, McpServerConfig& config,
                       QString& errorMessage);

class McpServerProcess final : public QObject
{
    Q_OBJECT

   public:
    explicit McpServerProcess(McpServerConfig config,
                              QObject* parent = nullptr);

    [[nodiscard]] const McpServerConfig& config() const;
    [[nodiscard]] QProcess::ProcessState state() const;
    [[nodiscard]] bool isRunning() const;

    void start();
    void stop();
    void cancelPendingShutdown();
    qint64 write(const QByteArray& data);
    QByteArray readStandardOutput();
    QByteArray readStandardError();

   signals:
    void started();
    void readyReadStandardOutput();
    void readyReadStandardError();
    void finished(int exitCode, QProcess::ExitStatus exitStatus);
    void errorOccurred(QProcess::ProcessError error, const QString& message);

   private:
    void onProcessError(QProcess::ProcessError error);

    McpServerConfig config_;
    QProcess process_;
    bool stopping_ = false;
};
}  // namespace qtllm::infrastructure::mcp

Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::McpServerConfig)
