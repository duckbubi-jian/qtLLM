#pragma once

#include "McpCatalog.hpp"
#include "McpServerRegistry.hpp"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
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

struct McpResourcePresentation
{
    QString uri;
    QString name;
    QString mimeType;
    bool resourceTemplate = false;
    bool subscribed = false;
};

struct McpPromptPresentation
{
    QString name;
    QString description;
    QList<infrastructure::mcp::McpPromptArgument> arguments;
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
    QString loggingLevel;
    QString appliedLoggingLevel;
    bool useInstructions = true;
    QString instructionsSource;
    QList<McpToolPresentation> tools;
    QList<McpResourcePresentation> resources;
    QList<McpPromptPresentation> prompts;
    QStringList diagnostics;
};

class McpControlPanel final : public QDialog
{
    Q_OBJECT

   public:
    explicit McpControlPanel(QWidget* parent = nullptr);

    void setServers(const QList<McpServerControlPresentation>& servers);
    void showResourceResult(
        const infrastructure::mcp::McpResourceReadResult& result);
    void showPromptResult(const infrastructure::mcp::McpPromptResult& result);
    void showCompletionResult(
        const infrastructure::mcp::McpCompletionResult& result);

   signals:
    void addServerRequested();
    void removeServerRequested(const QString& serverId);
    void serverEnabledChanged(const QString& serverId, bool builtIn,
                              bool enabled);
    void instructionsEnabledChanged(const QString& serverId, bool builtIn,
                                    bool enabled);
    void loggingLevelRequested(const QString& serverId, const QString& level);
    void startServerRequested(const QString& serverId);
    void stopServerRequested(const QString& serverId);
    void restartServerRequested(const QString& serverId);
    void pingServerRequested(const QString& serverId);
    void refreshToolsRequested(const QString& serverId);
    void readResourceRequested(const QString& serverId, const QString& uri);
    void resourceSubscriptionRequested(const QString& serverId,
                                       const QString& uri, bool subscribe);
    void getPromptRequested(const QString& serverId, const QString& name,
                            const QJsonObject& arguments);
    void completePromptRequested(const QString& serverId, const QString& name,
                                 const QString& argumentName,
                                 const QString& value,
                                 const QJsonObject& contextArguments);
    void completeResourceTemplateRequested(const QString& serverId,
                                           const QString& uriTemplate,
                                           const QString& argumentName,
                                           const QString& value);

   private:
    void selectServer(const QString& serverId);
    void updateDetails();
    void updateCatalogActions();
    [[nodiscard]] const McpServerControlPresentation* selectedServer() const;

    QList<McpServerControlPresentation> servers_;
    QTreeWidget* serverList_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    QLabel* serverNameLabel_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QCheckBox* enabledCheckBox_ = nullptr;
    QCheckBox* useInstructionsCheckBox_ = nullptr;
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
    QLabel* instructionsSourceValue_ = nullptr;
    QComboBox* loggingLevelCombo_ = nullptr;
    QLabel* loggingStatusValue_ = nullptr;
    QLabel* lastErrorValue_ = nullptr;
    QTreeWidget* toolsList_ = nullptr;
    QTreeWidget* resourcesList_ = nullptr;
    QPlainTextEdit* resourceResultEdit_ = nullptr;
    QToolButton* readResourceButton_ = nullptr;
    QToolButton* subscribeResourceButton_ = nullptr;
    QComboBox* resourceCompletionArgument_ = nullptr;
    QLineEdit* resourceCompletionValue_ = nullptr;
    QToolButton* completeResourceButton_ = nullptr;
    QPlainTextEdit* resourceCompletionResultEdit_ = nullptr;
    QTreeWidget* promptsList_ = nullptr;
    QLineEdit* promptArgumentsEdit_ = nullptr;
    QPlainTextEdit* promptResultEdit_ = nullptr;
    QToolButton* getPromptButton_ = nullptr;
    QComboBox* promptCompletionArgument_ = nullptr;
    QLineEdit* promptCompletionValue_ = nullptr;
    QToolButton* completePromptButton_ = nullptr;
    QPlainTextEdit* promptCompletionResultEdit_ = nullptr;
    QPlainTextEdit* diagnosticsEdit_ = nullptr;
    QToolButton* startButton_ = nullptr;
    QToolButton* stopButton_ = nullptr;
    QToolButton* restartButton_ = nullptr;
    QToolButton* pingButton_ = nullptr;
    QToolButton* refreshButton_ = nullptr;
};
}  // namespace qtllm::ui
