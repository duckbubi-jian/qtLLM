#pragma once

#include "McpServerRegistry.hpp"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace qtllm::ui
{
struct McpToolPresentation
{
    QString name;
    QString localPolicy;
    QString serverHints;
};

struct McpServerControlPresentation
{
    QString serverId;
    QString displayName;
    bool enabled = false;
    bool builtIn = false;
    bool available = true;
    bool registered = false;
    bool controlsEnabled = true;
    infrastructure::mcp::McpServerSnapshot snapshot;
    QString transport;
    QString serverInformation;
    QString program;
    QString workingDirectory;
    QString configurationSummary;
    QString allowlist;
    QString authorizedRoots;
    QList<McpToolPresentation> tools;
    QStringList diagnostics;
};

class McpControlPanel final : public QDialog
{
    Q_OBJECT

   public:
    explicit McpControlPanel(QWidget* parent = nullptr);

    void setServers(const QList<McpServerControlPresentation>& servers);

   signals:
    void addServerRequested();
    void removeServerRequested(const QString& serverId);
    void serverEnabledChanged(const QString& serverId, bool builtIn,
                              bool enabled);
    void startServerRequested(const QString& serverId);
    void stopServerRequested(const QString& serverId);
    void restartServerRequested(const QString& serverId);
    void refreshToolsRequested(const QString& serverId);

   private:
    void selectServer(const QString& serverId);
    void updateDetails();
    [[nodiscard]] const McpServerControlPresentation* selectedServer() const;

    QList<McpServerControlPresentation> servers_;
    QTreeWidget* serverList_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    QLabel* serverNameLabel_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QCheckBox* enabledCheckBox_ = nullptr;
    QLabel* transportValue_ = nullptr;
    QLabel* protocolValue_ = nullptr;
    QLabel* capabilitiesValue_ = nullptr;
    QLabel* serverInfoValue_ = nullptr;
    QLineEdit* programValue_ = nullptr;
    QLineEdit* workingDirectoryValue_ = nullptr;
    QLabel* configurationValue_ = nullptr;
    QLabel* allowlistValue_ = nullptr;
    QLabel* rootsValue_ = nullptr;
    QPlainTextEdit* instructionsEdit_ = nullptr;
    QLabel* lastErrorValue_ = nullptr;
    QTreeWidget* toolsList_ = nullptr;
    QPlainTextEdit* diagnosticsEdit_ = nullptr;
    QToolButton* startButton_ = nullptr;
    QToolButton* stopButton_ = nullptr;
    QToolButton* restartButton_ = nullptr;
    QToolButton* refreshButton_ = nullptr;
};
}  // namespace qtllm::ui
