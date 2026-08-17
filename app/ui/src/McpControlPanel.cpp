#include "McpControlPanel.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QTextCursor>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace qtllm::ui
{
namespace
{
QString stateText(const McpServerControlPresentation& server)
{
    using infrastructure::mcp::McpServerState;
    if (!server.available) return McpControlPanel::tr("Unavailable");
    if (!server.enabled) return McpControlPanel::tr("Off");
    if (!server.registered) return McpControlPanel::tr("Unavailable");
    switch (server.snapshot.state)
    {
        case McpServerState::Stopped:
            return McpControlPanel::tr("Stopped");
        case McpServerState::Starting:
            return McpControlPanel::tr("Starting");
        case McpServerState::Initializing:
            return McpControlPanel::tr("Initializing");
        case McpServerState::Ready:
            return McpControlPanel::tr("Ready");
        case McpServerState::Degraded:
            return McpControlPanel::tr("Degraded");
        case McpServerState::Failed:
            return McpControlPanel::tr("Failed");
        case McpServerState::Stopping:
            return McpControlPanel::tr("Stopping");
    }
    return McpControlPanel::tr("Unknown");
}

QString stateKey(const McpServerControlPresentation& server)
{
    if (!server.available || (server.enabled && !server.registered))
        return QStringLiteral("failed");
    if (!server.enabled) return QStringLiteral("off");
    return infrastructure::mcp::mcpServerStateName(server.snapshot.state);
}

QString capabilityText(
    const infrastructure::mcp::McpServerCapabilities& capabilities)
{
    QStringList names;
    if (capabilities.tools) names.append(McpControlPanel::tr("Tools"));
    if (capabilities.resources) names.append(McpControlPanel::tr("Resources"));
    if (capabilities.prompts) names.append(McpControlPanel::tr("Prompts"));
    if (capabilities.logging) names.append(McpControlPanel::tr("Logging"));
    if (capabilities.completions)
        names.append(McpControlPanel::tr("Completions"));
    return names.isEmpty() ? McpControlPanel::tr("None")
                           : names.join(QStringLiteral(", "));
}

QToolButton* actionButton(QWidget* parent, const QString& objectName,
                          const QString& text, QStyle::StandardPixmap icon)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setText(text);
    button->setIcon(button->style()->standardIcon(icon));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setToolTip(text);
    return button;
}
}  // namespace

McpControlPanel::McpControlPanel(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("mcpControlPanel"));
    setWindowTitle(tr("MCP servers"));
    setModal(false);
    resize(900, 590);
    setMinimumSize(700, 460);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("mcpControlSplitter"));
    splitter->setChildrenCollapsible(false);
    rootLayout->addWidget(splitter, 1);

    auto* serverPane = new QWidget(splitter);
    auto* serverLayout = new QVBoxLayout(serverPane);
    serverLayout->setContentsMargins(0, 0, 8, 0);
    serverLayout->setSpacing(6);
    serverList_ = new QTreeWidget(serverPane);
    serverList_->setObjectName(QStringLiteral("mcpServerList"));
    serverList_->setColumnCount(3);
    serverList_->setHeaderLabels({tr("Server"), tr("State"), tr("Tools")});
    serverList_->setRootIsDecorated(false);
    serverList_->setAlternatingRowColors(true);
    serverList_->setSelectionMode(QAbstractItemView::SingleSelection);
    serverList_->setUniformRowHeights(true);
    serverList_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    serverList_->header()->setSectionResizeMode(1,
                                                QHeaderView::ResizeToContents);
    serverList_->header()->setSectionResizeMode(2,
                                                QHeaderView::ResizeToContents);
    serverLayout->addWidget(serverList_, 1);

    auto* serverActions = new QHBoxLayout;
    serverActions->setContentsMargins(0, 0, 0, 0);
    addButton_ = actionButton(serverPane, QStringLiteral("addMcpServerButton"),
                              tr("Add"), QStyle::SP_FileDialogNewFolder);
    removeButton_ =
        actionButton(serverPane, QStringLiteral("removeMcpServerButton"),
                     tr("Remove"), QStyle::SP_TrashIcon);
    serverActions->addWidget(addButton_);
    serverActions->addWidget(removeButton_);
    serverActions->addStretch(1);
    serverLayout->addLayout(serverActions);

    auto* detailPane = new QWidget(splitter);
    auto* detailLayout = new QVBoxLayout(detailPane);
    detailLayout->setContentsMargins(8, 0, 0, 0);
    detailLayout->setSpacing(8);

    auto* headingLayout = new QHBoxLayout;
    serverNameLabel_ = new QLabel(detailPane);
    serverNameLabel_->setObjectName(QStringLiteral("mcpServerName"));
    stateLabel_ = new QLabel(detailPane);
    stateLabel_->setObjectName(QStringLiteral("mcpServerState"));
    enabledCheckBox_ = new QCheckBox(tr("Enabled"), detailPane);
    enabledCheckBox_->setObjectName(QStringLiteral("mcpServerEnabled"));
    headingLayout->addWidget(serverNameLabel_, 1);
    headingLayout->addWidget(stateLabel_);
    headingLayout->addWidget(enabledCheckBox_);
    detailLayout->addLayout(headingLayout);

    auto* tabs = new QTabWidget(detailPane);
    tabs->setObjectName(QStringLiteral("mcpControlTabs"));
    detailLayout->addWidget(tabs, 1);

    auto* overviewPage = new QWidget(tabs);
    auto* overviewLayout = new QFormLayout(overviewPage);
    overviewLayout->setContentsMargins(10, 10, 10, 10);
    overviewLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    transportValue_ = new QLabel(overviewPage);
    protocolValue_ = new QLabel(overviewPage);
    capabilitiesValue_ = new QLabel(overviewPage);
    capabilitiesValue_->setWordWrap(true);
    serverInfoValue_ = new QLabel(overviewPage);
    serverInfoValue_->setWordWrap(true);
    programValue_ = new QLineEdit(overviewPage);
    programValue_->setObjectName(QStringLiteral("mcpProgramValue"));
    programValue_->setReadOnly(true);
    workingDirectoryValue_ = new QLineEdit(overviewPage);
    workingDirectoryValue_->setObjectName(
        QStringLiteral("mcpWorkingDirectoryValue"));
    workingDirectoryValue_->setReadOnly(true);
    configurationValue_ = new QLabel(overviewPage);
    configurationValue_->setWordWrap(true);
    allowlistValue_ = new QLabel(overviewPage);
    allowlistValue_->setWordWrap(true);
    rootsValue_ = new QLabel(overviewPage);
    rootsValue_->setWordWrap(true);
    instructionsEdit_ = new QPlainTextEdit(overviewPage);
    instructionsEdit_->setObjectName(QStringLiteral("mcpInstructions"));
    instructionsEdit_->setReadOnly(true);
    instructionsEdit_->setMaximumHeight(96);
    lastErrorValue_ = new QLabel(overviewPage);
    lastErrorValue_->setObjectName(QStringLiteral("mcpLastError"));
    lastErrorValue_->setWordWrap(true);
    overviewLayout->addRow(tr("Transport"), transportValue_);
    overviewLayout->addRow(tr("Protocol"), protocolValue_);
    overviewLayout->addRow(tr("Capabilities"), capabilitiesValue_);
    overviewLayout->addRow(tr("Server info"), serverInfoValue_);
    overviewLayout->addRow(tr("Program"), programValue_);
    overviewLayout->addRow(tr("Working directory"), workingDirectoryValue_);
    overviewLayout->addRow(tr("Configuration"), configurationValue_);
    overviewLayout->addRow(tr("Tool allowlist"), allowlistValue_);
    overviewLayout->addRow(tr("Authorized roots"), rootsValue_);
    overviewLayout->addRow(tr("Instructions"), instructionsEdit_);
    overviewLayout->addRow(tr("Last error"), lastErrorValue_);
    tabs->addTab(overviewPage, tr("Overview"));

    toolsList_ = new QTreeWidget(tabs);
    toolsList_->setObjectName(QStringLiteral("mcpToolList"));
    toolsList_->setColumnCount(3);
    toolsList_->setHeaderLabels(
        {tr("Tool"), tr("Local policy"), tr("Server hints")});
    toolsList_->setRootIsDecorated(false);
    toolsList_->setAlternatingRowColors(true);
    toolsList_->setUniformRowHeights(true);
    toolsList_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    toolsList_->header()->setSectionResizeMode(1,
                                               QHeaderView::ResizeToContents);
    toolsList_->header()->setSectionResizeMode(2,
                                               QHeaderView::ResizeToContents);
    tabs->addTab(toolsList_, tr("Tools"));

    diagnosticsEdit_ = new QPlainTextEdit(tabs);
    diagnosticsEdit_->setObjectName(QStringLiteral("mcpDiagnostics"));
    diagnosticsEdit_->setReadOnly(true);
    diagnosticsEdit_->setMaximumBlockCount(200);
    tabs->addTab(diagnosticsEdit_, tr("Diagnostics"));

    auto* runtimeActions = new QHBoxLayout;
    startButton_ =
        actionButton(detailPane, QStringLiteral("startMcpServerButton"),
                     tr("Start"), QStyle::SP_MediaPlay);
    stopButton_ =
        actionButton(detailPane, QStringLiteral("stopMcpServerButton"),
                     tr("Stop"), QStyle::SP_MediaStop);
    restartButton_ =
        actionButton(detailPane, QStringLiteral("restartMcpServerButton"),
                     tr("Restart"), QStyle::SP_BrowserReload);
    refreshButton_ =
        actionButton(detailPane, QStringLiteral("refreshMcpToolsButton"),
                     tr("Refresh tools"), QStyle::SP_DialogApplyButton);
    runtimeActions->addWidget(startButton_);
    runtimeActions->addWidget(stopButton_);
    runtimeActions->addWidget(restartButton_);
    runtimeActions->addStretch(1);
    runtimeActions->addWidget(refreshButton_);
    detailLayout->addLayout(runtimeActions);

    splitter->addWidget(serverPane);
    splitter->addWidget(detailPane);
    splitter->setSizes({280, 600});

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->setObjectName(QStringLiteral("mcpControlButtons"));
    rootLayout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(serverList_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem*, QTreeWidgetItem*) { updateDetails(); });
    connect(addButton_, &QToolButton::clicked, this,
            &McpControlPanel::addServerRequested);
    connect(removeButton_, &QToolButton::clicked, this,
            [this]
            {
                if (const auto* server = selectedServer())
                    emit removeServerRequested(server->serverId);
            });
    connect(enabledCheckBox_, &QCheckBox::toggled, this,
            [this](bool enabled)
            {
                if (const auto* server = selectedServer())
                    emit serverEnabledChanged(server->serverId, server->builtIn,
                                              enabled);
            });
    connect(startButton_, &QToolButton::clicked, this,
            [this]
            {
                if (const auto* server = selectedServer())
                    emit startServerRequested(server->serverId);
            });
    connect(stopButton_, &QToolButton::clicked, this,
            [this]
            {
                if (const auto* server = selectedServer())
                    emit stopServerRequested(server->serverId);
            });
    connect(restartButton_, &QToolButton::clicked, this,
            [this]
            {
                if (const auto* server = selectedServer())
                    emit restartServerRequested(server->serverId);
            });
    connect(refreshButton_, &QToolButton::clicked, this,
            [this]
            {
                if (const auto* server = selectedServer())
                    emit refreshToolsRequested(server->serverId);
            });

    updateDetails();
}

void McpControlPanel::setServers(
    const QList<McpServerControlPresentation>& servers)
{
    const auto selectedId =
        selectedServer() == nullptr ? QString{} : selectedServer()->serverId;
    servers_ = servers;
    std::sort(servers_.begin(), servers_.end(),
              [](const auto& left, const auto& right)
              {
                  if (left.builtIn != right.builtIn) return left.builtIn;
                  return QString::compare(left.displayName, right.displayName,
                                          Qt::CaseInsensitive) < 0;
              });
    serverList_->clear();
    for (const auto& server : servers_)
    {
        auto* item =
            new QTreeWidgetItem({server.displayName, stateText(server),
                                 QString::number(server.snapshot.toolCount)});
        item->setData(0, Qt::UserRole, server.serverId);
        item->setToolTip(0, server.serverId);
        serverList_->addTopLevelItem(item);
    }
    selectServer(selectedId);
    if (serverList_->currentItem() == nullptr &&
        serverList_->topLevelItemCount() > 0)
        serverList_->setCurrentItem(serverList_->topLevelItem(0));
    updateDetails();
}

void McpControlPanel::selectServer(const QString& serverId)
{
    if (serverId.isEmpty()) return;
    for (auto index = 0; index < serverList_->topLevelItemCount(); ++index)
    {
        auto* item = serverList_->topLevelItem(index);
        if (item->data(0, Qt::UserRole).toString() == serverId)
        {
            serverList_->setCurrentItem(item);
            return;
        }
    }
}

const McpServerControlPresentation* McpControlPanel::selectedServer() const
{
    const auto* item =
        serverList_ == nullptr ? nullptr : serverList_->currentItem();
    if (item == nullptr) return nullptr;
    const auto serverId = item->data(0, Qt::UserRole).toString();
    const auto iterator = std::find_if(servers_.cbegin(), servers_.cend(),
                                       [&serverId](const auto& server)
                                       { return server.serverId == serverId; });
    return iterator == servers_.cend() ? nullptr : &*iterator;
}

void McpControlPanel::updateDetails()
{
    const auto* server = selectedServer();
    const auto hasServer = server != nullptr;
    serverNameLabel_->setText(hasServer ? server->displayName
                                        : tr("No server"));
    stateLabel_->setText(hasServer ? stateText(*server) : QString{});
    stateLabel_->setProperty(
        "serverState", hasServer ? stateKey(*server) : QStringLiteral("off"));
    stateLabel_->style()->unpolish(stateLabel_);
    stateLabel_->style()->polish(stateLabel_);

    const QSignalBlocker blocker(enabledCheckBox_);
    enabledCheckBox_->setChecked(hasServer && server->enabled);
    enabledCheckBox_->setEnabled(hasServer && server->available &&
                                 server->controlsEnabled);
    removeButton_->setEnabled(hasServer && !server->builtIn &&
                              server->controlsEnabled);

    transportValue_->setText(hasServer ? server->transport : QString{});
    protocolValue_->setText(hasServer &&
                                    !server->snapshot.protocolVersion.isEmpty()
                                ? server->snapshot.protocolVersion
                                : tr("Not negotiated"));
    capabilitiesValue_->setText(
        hasServer ? capabilityText(server->snapshot.capabilities) : QString{});
    serverInfoValue_->setText(hasServer ? server->serverInformation
                                        : QString{});
    programValue_->setText(hasServer ? server->program : QString{});
    workingDirectoryValue_->setText(hasServer ? server->workingDirectory
                                              : QString{});
    configurationValue_->setText(hasServer ? server->configurationSummary
                                           : QString{});
    allowlistValue_->setText(hasServer ? server->allowlist : QString{});
    rootsValue_->setText(hasServer ? server->authorizedRoots : QString{});
    instructionsEdit_->setPlainText(hasServer ? server->snapshot.instructions
                                              : QString{});
    auto error = QString{};
    if (hasServer && !server->snapshot.lastErrorMessage.isEmpty())
        error = server->snapshot.lastErrorCode.isEmpty()
                    ? server->snapshot.lastErrorMessage
                    : QStringLiteral("%1: %2").arg(
                          server->snapshot.lastErrorCode,
                          server->snapshot.lastErrorMessage);
    lastErrorValue_->setText(error.isEmpty() ? tr("None") : error);

    toolsList_->clear();
    if (hasServer)
        for (const auto& tool : server->tools)
            toolsList_->addTopLevelItem(new QTreeWidgetItem(
                {tool.name, tool.localPolicy, tool.serverHints}));
    diagnosticsEdit_->setPlainText(
        hasServer ? server->diagnostics.join(QLatin1Char('\n')) : QString{});
    if (hasServer)
    {
        auto cursor = diagnosticsEdit_->textCursor();
        cursor.movePosition(QTextCursor::End);
        diagnosticsEdit_->setTextCursor(cursor);
    }

    using infrastructure::mcp::McpServerState;
    const auto state =
        hasServer ? server->snapshot.state : McpServerState::Stopped;
    const auto controls = hasServer && server->controlsEnabled &&
                          server->available && server->enabled &&
                          server->registered;
    const auto busy = state == McpServerState::Starting ||
                      state == McpServerState::Initializing ||
                      state == McpServerState::Stopping;
    startButton_->setEnabled(controls && (state == McpServerState::Stopped ||
                                          state == McpServerState::Failed));
    stopButton_->setEnabled(controls && state != McpServerState::Stopped &&
                            state != McpServerState::Stopping);
    restartButton_->setEnabled(controls && !busy);
    refreshButton_->setEnabled(
        controls && server->snapshot.capabilities.tools &&
        (state == McpServerState::Ready || state == McpServerState::Degraded));
    addButton_->setEnabled(!hasServer || server->controlsEnabled);
}
}  // namespace qtllm::ui
