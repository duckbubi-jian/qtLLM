#include "McpControlPanel.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonParseError>
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

    auto* resourcesPage = new QWidget(tabs);
    auto* resourcesLayout = new QVBoxLayout(resourcesPage);
    resourcesLayout->setContentsMargins(8, 8, 8, 8);
    resourcesLayout->setSpacing(6);
    resourcesList_ = new QTreeWidget(resourcesPage);
    resourcesList_->setObjectName(QStringLiteral("mcpResourceList"));
    resourcesList_->setColumnCount(3);
    resourcesList_->setHeaderLabels(
        {tr("Resource"), tr("URI or template"), tr("MIME")});
    resourcesList_->setRootIsDecorated(false);
    resourcesList_->setAlternatingRowColors(true);
    resourcesList_->setUniformRowHeights(true);
    resourcesList_->header()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    resourcesList_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    resourcesList_->header()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    resourcesLayout->addWidget(resourcesList_, 1);
    resourceResultEdit_ = new QPlainTextEdit(resourcesPage);
    resourceResultEdit_->setObjectName(QStringLiteral("mcpResourceResult"));
    resourceResultEdit_->setReadOnly(true);
    resourceResultEdit_->setMaximumBlockCount(500);
    resourceResultEdit_->setMaximumHeight(150);
    resourcesLayout->addWidget(resourceResultEdit_);
    auto* resourceActions = new QHBoxLayout;
    resourceActions->addStretch(1);
    subscribeResourceButton_ = actionButton(
        resourcesPage, QStringLiteral("subscribeMcpResourceButton"),
        tr("Subscribe"), QStyle::SP_DialogApplyButton);
    readResourceButton_ =
        actionButton(resourcesPage, QStringLiteral("readMcpResourceButton"),
                     tr("Read"), QStyle::SP_FileIcon);
    resourceActions->addWidget(subscribeResourceButton_);
    resourceActions->addWidget(readResourceButton_);
    resourcesLayout->addLayout(resourceActions);
    tabs->addTab(resourcesPage, tr("Resources"));

    auto* promptsPage = new QWidget(tabs);
    auto* promptsLayout = new QVBoxLayout(promptsPage);
    promptsLayout->setContentsMargins(8, 8, 8, 8);
    promptsLayout->setSpacing(6);
    promptsList_ = new QTreeWidget(promptsPage);
    promptsList_->setObjectName(QStringLiteral("mcpPromptList"));
    promptsList_->setColumnCount(3);
    promptsList_->setHeaderLabels(
        {tr("Prompt"), tr("Description"), tr("Arguments")});
    promptsList_->setRootIsDecorated(false);
    promptsList_->setAlternatingRowColors(true);
    promptsList_->setUniformRowHeights(true);
    promptsList_->header()->setSectionResizeMode(0,
                                                 QHeaderView::ResizeToContents);
    promptsList_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    promptsList_->header()->setSectionResizeMode(2,
                                                 QHeaderView::ResizeToContents);
    promptsLayout->addWidget(promptsList_, 1);
    auto* promptArgumentsLayout = new QHBoxLayout;
    auto* promptArgumentsLabel =
        new QLabel(tr("Arguments (JSON)"), promptsPage);
    promptArgumentsEdit_ = new QLineEdit(promptsPage);
    promptArgumentsEdit_->setObjectName(QStringLiteral("mcpPromptArguments"));
    promptArgumentsLayout->addWidget(promptArgumentsLabel);
    promptArgumentsLayout->addWidget(promptArgumentsEdit_, 1);
    promptsLayout->addLayout(promptArgumentsLayout);
    promptResultEdit_ = new QPlainTextEdit(promptsPage);
    promptResultEdit_->setObjectName(QStringLiteral("mcpPromptResult"));
    promptResultEdit_->setReadOnly(true);
    promptResultEdit_->setMaximumBlockCount(500);
    promptResultEdit_->setMaximumHeight(150);
    promptsLayout->addWidget(promptResultEdit_);
    auto* promptActions = new QHBoxLayout;
    promptActions->addStretch(1);
    getPromptButton_ =
        actionButton(promptsPage, QStringLiteral("getMcpPromptButton"),
                     tr("Get prompt"), QStyle::SP_DialogApplyButton);
    promptActions->addWidget(getPromptButton_);
    promptsLayout->addLayout(promptActions);
    tabs->addTab(promptsPage, tr("Prompts"));

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
    pingButton_ =
        actionButton(detailPane, QStringLiteral("pingMcpServerButton"),
                     tr("Ping"), QStyle::SP_ComputerIcon);
    refreshButton_ =
        actionButton(detailPane, QStringLiteral("refreshMcpToolsButton"),
                     tr("Refresh catalogs"), QStyle::SP_DialogApplyButton);
    runtimeActions->addWidget(startButton_);
    runtimeActions->addWidget(stopButton_);
    runtimeActions->addWidget(restartButton_);
    runtimeActions->addStretch(1);
    runtimeActions->addWidget(pingButton_);
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
    connect(pingButton_, &QToolButton::clicked, this,
            [this]
            {
                if (const auto* server = selectedServer())
                    emit pingServerRequested(server->serverId);
            });
    connect(refreshButton_, &QToolButton::clicked, this,
            [this]
            {
                if (const auto* server = selectedServer())
                    emit refreshToolsRequested(server->serverId);
            });
    connect(resourcesList_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem*, QTreeWidgetItem*)
            { updateCatalogActions(); });
    connect(promptsList_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*)
            {
                QJsonObject arguments;
                if (current != nullptr)
                {
                    const auto* server = selectedServer();
                    const auto name = current->data(0, Qt::UserRole).toString();
                    if (server != nullptr)
                        for (const auto& prompt : server->prompts)
                            if (prompt.name == name)
                                for (const auto& argument : prompt.arguments)
                                    if (argument.required)
                                        arguments.insert(argument.name,
                                                         QString{});
                }
                promptArgumentsEdit_->setText(QString::fromUtf8(
                    QJsonDocument(arguments).toJson(QJsonDocument::Compact)));
                updateCatalogActions();
            });
    connect(readResourceButton_, &QToolButton::clicked, this,
            [this]
            {
                const auto* server = selectedServer();
                const auto* resource = resourcesList_->currentItem();
                if (server != nullptr && resource != nullptr)
                    emit readResourceRequested(
                        server->serverId,
                        resource->data(0, Qt::UserRole).toString());
            });
    connect(subscribeResourceButton_, &QToolButton::clicked, this,
            [this]
            {
                const auto* server = selectedServer();
                const auto* resource = resourcesList_->currentItem();
                if (server != nullptr && resource != nullptr)
                    emit resourceSubscriptionRequested(
                        server->serverId,
                        resource->data(0, Qt::UserRole).toString(),
                        !resource->data(0, Qt::UserRole + 2).toBool());
            });
    connect(getPromptButton_, &QToolButton::clicked, this,
            [this]
            {
                const auto* server = selectedServer();
                const auto* prompt = promptsList_->currentItem();
                if (server == nullptr || prompt == nullptr) return;
                QJsonParseError error;
                const auto arguments = QJsonDocument::fromJson(
                    promptArgumentsEdit_->text().toUtf8(), &error);
                if (error.error != QJsonParseError::NoError ||
                    !arguments.isObject())
                {
                    promptResultEdit_->setPlainText(
                        tr("Prompt arguments must be a JSON object."));
                    return;
                }
                emit getPromptRequested(
                    server->serverId, prompt->data(0, Qt::UserRole).toString(),
                    arguments.object());
            });

    updateDetails();
}

void McpControlPanel::showResourceResult(
    const infrastructure::mcp::McpResourceReadResult& result)
{
    const auto* server = selectedServer();
    if (server == nullptr || server->serverId != result.serverId) return;
    QStringList text;
    for (const auto& content : result.contents)
    {
        text.append(QStringLiteral("[%1] %2").arg(
            content.uri, content.mimeType.isEmpty() ? tr("unspecified MIME")
                                                    : content.mimeType));
        text.append(content.binary ? tr("Binary content: %n Base64 bytes",
                                        nullptr, content.blob.size())
                                   : content.text);
    }
    resourceResultEdit_->setPlainText(text.join(QStringLiteral("\n\n")));
}

void McpControlPanel::showPromptResult(
    const infrastructure::mcp::McpPromptResult& result)
{
    const auto* server = selectedServer();
    if (server == nullptr || server->serverId != result.serverId) return;
    QStringList text;
    if (!result.description.isEmpty()) text.append(result.description);
    for (const auto& message : result.messages)
    {
        const auto type =
            message.content.value(QStringLiteral("type")).toString();
        text.append(QStringLiteral("[%1 / %2]").arg(message.role, type));
        if (type == QLatin1String("text"))
            text.append(
                message.content.value(QStringLiteral("text")).toString());
        else
            text.append(QString::fromUtf8(
                QJsonDocument(message.content).toJson(QJsonDocument::Compact)));
    }
    promptResultEdit_->setPlainText(text.join(QStringLiteral("\n\n")));
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

    resourcesList_->clear();
    if (hasServer)
        for (const auto& resource : server->resources)
        {
            auto* item = new QTreeWidgetItem({resource.name, resource.uri,
                                              resource.mimeType.isEmpty()
                                                  ? tr("Unspecified")
                                                  : resource.mimeType});
            item->setData(0, Qt::UserRole, resource.uri);
            item->setData(0, Qt::UserRole + 1, resource.resourceTemplate);
            item->setData(0, Qt::UserRole + 2, resource.subscribed);
            if (resource.resourceTemplate)
                item->setToolTip(0, tr("Resource template"));
            resourcesList_->addTopLevelItem(item);
        }
    if (resourcesList_->topLevelItemCount() > 0)
        resourcesList_->setCurrentItem(resourcesList_->topLevelItem(0));

    promptsList_->clear();
    if (hasServer)
        for (const auto& prompt : server->prompts)
        {
            QStringList arguments;
            for (const auto& argument : prompt.arguments)
                arguments.append(argument.required
                                     ? tr("%1 (required)").arg(argument.name)
                                     : argument.name);
            auto* item =
                new QTreeWidgetItem({prompt.name, prompt.description,
                                     arguments.join(QStringLiteral(", "))});
            item->setData(0, Qt::UserRole, prompt.name);
            promptsList_->addTopLevelItem(item);
        }
    if (promptsList_->topLevelItemCount() > 0)
        promptsList_->setCurrentItem(promptsList_->topLevelItem(0));
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
    pingButton_->setEnabled(controls && (state == McpServerState::Ready ||
                                         state == McpServerState::Degraded));
    refreshButton_->setEnabled(
        controls &&
        (server->snapshot.capabilities.tools ||
         server->snapshot.capabilities.resources ||
         server->snapshot.capabilities.prompts) &&
        (state == McpServerState::Ready || state == McpServerState::Degraded));
    addButton_->setEnabled(!hasServer || server->controlsEnabled);
    updateCatalogActions();
}

void McpControlPanel::updateCatalogActions()
{
    const auto* server = selectedServer();
    using infrastructure::mcp::McpServerState;
    const auto ready = server != nullptr && server->controlsEnabled &&
                       server->available && server->enabled &&
                       server->registered &&
                       (server->snapshot.state == McpServerState::Ready ||
                        server->snapshot.state == McpServerState::Degraded);
    const auto* resource = resourcesList_->currentItem();
    readResourceButton_->setEnabled(
        ready && resource != nullptr &&
        !resource->data(0, Qt::UserRole + 1).toBool());
    const auto subscribable =
        ready && server->snapshot.capabilities.resourcesSubscribe &&
        resource != nullptr && !resource->data(0, Qt::UserRole + 1).toBool();
    subscribeResourceButton_->setEnabled(subscribable);
    const auto subscribed =
        resource != nullptr && resource->data(0, Qt::UserRole + 2).toBool();
    subscribeResourceButton_->setText(subscribed ? tr("Unsubscribe")
                                                 : tr("Subscribe"));
    subscribeResourceButton_->setToolTip(subscribeResourceButton_->text());
    getPromptButton_->setEnabled(ready &&
                                 promptsList_->currentItem() != nullptr);
    promptArgumentsEdit_->setEnabled(ready &&
                                     promptsList_->currentItem() != nullptr);
}
}  // namespace qtllm::ui
