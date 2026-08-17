#include "ChatView.hpp"

#include "AutoHideTabWidget.hpp"
#include "ui_ChatView.h"

#include <QAction>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace qtllm::ui
{
namespace
{
QIcon primaryActionIcon(bool stopMode)
{
    constexpr auto logicalSize = 20;
    constexpr auto devicePixelRatio = 2.0;
    QPixmap pixmap(static_cast<int>(logicalSize * devicePixelRatio),
                   static_cast<int>(logicalSize * devicePixelRatio));
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    if (stopMode)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawRoundedRect(QRectF(6.0, 6.0, 8.0, 8.0), 1.0, 1.0);
    }
    else
    {
        painter.setPen(
            QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(10.0, 15.5), QPointF(10.0, 4.5));
        painter.drawLine(QPointF(5.5, 9.0), QPointF(10.0, 4.5));
        painter.drawLine(QPointF(14.5, 9.0), QPointF(10.0, 4.5));
    }

    QIcon icon;
    icon.addPixmap(pixmap, QIcon::Normal);
    icon.addPixmap(pixmap, QIcon::Disabled);
    return icon;
}

QString modelBadgeStateName(ModelBadgeState state)
{
    switch (state)
    {
        case ModelBadgeState::Neutral:
            return QStringLiteral("neutral");
        case ModelBadgeState::Checking:
            return QStringLiteral("checking");
        case ModelBadgeState::Verified:
            return QStringLiteral("verified");
        case ModelBadgeState::Unverified:
            return QStringLiteral("unverified");
        case ModelBadgeState::Invalid:
            return QStringLiteral("invalid");
    }
    return QStringLiteral("neutral");
}

QString modelBadgeTextColor(ModelBadgeState state)
{
    switch (state)
    {
        case ModelBadgeState::Checking:
            return QStringLiteral("#1d4ed8");
        case ModelBadgeState::Verified:
            return QStringLiteral("#166534");
        case ModelBadgeState::Unverified:
            return QStringLiteral("#9f1239");
        case ModelBadgeState::Invalid:
            return QStringLiteral("#991b1b");
        case ModelBadgeState::Neutral:
            return QStringLiteral("#475569");
    }
    return QStringLiteral("#475569");
}
}  // namespace

ChatView::ChatView(QWidget* parent)
    : QWidget(parent), ui_(std::make_unique<Ui::ChatView>())
{
    ui_->setupUi(this);

    ui_->activityLog->setFont(
        QFontDatabase::systemFont(QFontDatabase::FixedFont));
    ui_->clearConversationAction->setIcon(
        style()->standardIcon(QStyle::SP_DialogResetButton));
    ui_->modelReloadAction->setIcon(
        style()->standardIcon(QStyle::SP_BrowserReload));
    ui_->modelPathLink->addAction(ui_->modelReloadAction);
    ui_->workspacePathLink->addAction(ui_->workspaceChangeAction);
    ui_->transcriptTabs->addAction(ui_->clearConversationAction);
    ui_->transcriptTabs->tabBar()->setContextMenuPolicy(Qt::ActionsContextMenu);
    ui_->transcriptTabs->tabBar()->addAction(ui_->clearConversationAction);
    ui_->guideSelectModelButton->setIcon(
        style()->standardIcon(QStyle::SP_DirOpenIcon));
    ui_->guideLoadModelButton->setIcon(
        style()->standardIcon(QStyle::SP_MediaPlay));

    ui_->workspacePathLink->setSizePolicy(QSizePolicy::Preferred,
                                          QSizePolicy::Preferred);
    ui_->workspacePathLink->setCursor(Qt::PointingHandCursor);
    ui_->workspacePathLink->setFocusPolicy(Qt::StrongFocus);
    ui_->modelPathLink->setSizePolicy(QSizePolicy::Fixed,
                                      QSizePolicy::Preferred);
    ui_->workspacePathLink->installEventFilter(this);
    ui_->modelPathLink->installEventFilter(this);
    ui_->promptEditor->installEventFilter(this);

    mcpMenu_ = new QMenu(ui_->mcpMenuButton);
    mcpMenu_->setObjectName(QStringLiteral("mcpServerMenu"));
    ui_->mcpMenuButton->setMenu(mcpMenu_);
    ui_->mcpMenuButton->setText({});
    ui_->mcpMenuButton->setIcon(style()->standardIcon(QStyle::SP_DriveNetIcon));
    ui_->mcpMenuButton->setIconSize(QSize(16, 16));

    connect(ui_->modelReloadAction, &QAction::triggered, this,
            &ChatView::modelFolderRequested);
    connect(ui_->guideSelectModelButton, &QPushButton::clicked, this,
            &ChatView::modelFolderRequested);
    connect(ui_->modelPathLink, &QLabel::linkActivated, this,
            &ChatView::modelLocationRequested);
    connect(ui_->guideLoadModelButton, &QPushButton::clicked, this,
            &ChatView::modelLoadRequested);
    connect(ui_->workspaceChangeAction, &QAction::triggered, this,
            &ChatView::workspaceFolderRequested);
    connect(ui_->primaryActionButton, &QPushButton::clicked, this,
            &ChatView::primaryActionRequested);
    connect(ui_->clearConversationAction, &QAction::triggered, this,
            &ChatView::clearConversationRequested);
    connect(ui_->agentModeSwitch, &QCheckBox::toggled, this,
            [this](bool agentMode)
            {
                updateModePresentation();
                emit modeChanged(agentMode);
            });

    setModelPresentation({}, tr("Model not checked"), ModelBadgeState::Neutral);
    setWorkspacePresentation({});
    updateModePresentation();
    setPrimaryAction(false, false);
    setConversationVisible(false);
}

ChatView::~ChatView() = default;

QPlainTextEdit* ChatView::promptEditor() const
{
    return ui_->promptEditor;
}

QPlainTextEdit* ChatView::activityLog() const
{
    return ui_->activityLog;
}

QScrollArea* ChatView::conversationScroll() const
{
    return ui_->conversationScroll;
}

QVBoxLayout* ChatView::conversationLayout() const
{
    return ui_->conversationLayout;
}

bool ChatView::isAgentModeSelected() const
{
    return ui_->agentModeSwitch->isChecked();
}

void ChatView::setModelPresentation(const QString& modelPath,
                                    const QString& modelInformation,
                                    ModelBadgeState state)
{
    modelPath_ = modelPath;
    modelInformation_ = modelInformation;
    modelBadgeState_ = state;
    const auto stateName = modelBadgeStateName(state);
    for (auto* label : {ui_->modelPathLink, ui_->guideModelNameLabel})
    {
        label->setProperty("modelState", stateName);
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
    updateModelPresentation();
}

void ChatView::setWorkspacePresentation(const QString& workspacePath)
{
    workspacePath_ = workspacePath;
    updateWorkspacePresentation();
}

void ChatView::setModelControlsEnabled(bool selectionEnabled, bool loadEnabled)
{
    ui_->modelReloadAction->setEnabled(selectionEnabled);
    ui_->guideSelectModelButton->setEnabled(selectionEnabled);
    ui_->guideLoadModelButton->setEnabled(loadEnabled);
}

void ChatView::setWorkspaceControlsEnabled(bool enabled)
{
    ui_->workspaceChangeAction->setEnabled(enabled);
    ui_->workspacePathLink->setEnabled(!workspacePath_.isEmpty());
}

void ChatView::setPromptEnabled(bool enabled)
{
    ui_->promptEditor->setEnabled(enabled);
}

void ChatView::setClearEnabled(bool enabled)
{
    ui_->clearConversationAction->setEnabled(enabled);
}

void ChatView::setPrimaryAction(bool stopMode, bool enabled)
{
    primaryActionStops_ = stopMode;
    ui_->primaryActionButton->setProperty("stopMode", stopMode);
    ui_->primaryActionButton->setIcon(primaryActionIcon(stopMode));
    ui_->primaryActionButton->setIconSize(QSize(20, 20));
    ui_->primaryActionButton->setToolTip(stopMode ? tr("Stop generation")
                                                  : tr("Send message"));
    ui_->primaryActionButton->setAccessibleName(stopMode ? tr("Stop generation")
                                                         : tr("Send message"));
    ui_->primaryActionButton->setDefault(!stopMode);
    ui_->primaryActionButton->setEnabled(enabled);
    ui_->primaryActionButton->style()->unpolish(ui_->primaryActionButton);
    ui_->primaryActionButton->style()->polish(ui_->primaryActionButton);
}

void ChatView::setModeSelectionEnabled(bool enabled)
{
    ui_->agentModeSwitch->setEnabled(enabled);
}

void ChatView::setMcpSelectionEnabled(bool enabled)
{
    ui_->mcpMenuButton->setEnabled(enabled);
}

void ChatView::setMcpServers(const QList<McpServerPresentation>& servers)
{
    mcpServers_ = servers;
    rebuildMcpMenu();
}

void ChatView::setAgentModeSelected(bool selected)
{
    const QSignalBlocker blocker(ui_->agentModeSwitch);
    ui_->agentModeSwitch->setChecked(selected);
    updateModePresentation();
}

void ChatView::setConversationVisible(bool visible)
{
    ui_->contentStack->setCurrentWidget(visible ? ui_->transcriptPage
                                                : ui_->modelGuidePage);
    ui_->promptComposer->setVisible(visible);
    ui_->statusLabel->setVisible(visible);
}

void ChatView::setStatusText(const QString& text)
{
    ui_->statusLabel->setText(text);
    ui_->guideStatusLabel->setText(text);
    ui_->guideStatusLabel->setVisible(!text.isEmpty());
}

void ChatView::updateModePresentation()
{
    const auto agentMode = ui_->agentModeSwitch->isChecked();
    ui_->mcpMenuButton->setVisible(agentMode);
    ui_->promptComposer->setProperty("agentMode", agentMode);
    ui_->promptComposer->style()->unpolish(ui_->promptComposer);
    ui_->promptComposer->style()->polish(ui_->promptComposer);
    ui_->promptEditor->setPlaceholderText(
        agentMode
            ? tr("Describe a task for the agent (Shift+Enter for a new line)")
            : tr("Write a message (Shift+Enter for a new line)"));
    ui_->agentModeSwitch->setToolTip(agentMode ? tr("Switch to Chat mode")
                                               : tr("Switch to Agent mode"));
}

void ChatView::rebuildMcpMenu()
{
    mcpMenu_->clear();
    auto* heading = mcpMenu_->addAction(tr("MCP servers"));
    heading->setEnabled(false);

    auto* manageAction = mcpMenu_->addAction(tr("Manage MCP servers..."));
    manageAction->setObjectName(QStringLiteral("manageMcpServersAction"));
    manageAction->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    connect(manageAction, &QAction::triggered, this,
            &ChatView::manageMcpServersRequested);
    mcpMenu_->addSeparator();

    for (const auto& server : mcpServers_)
    {
        auto label = server.displayName;
        if (!server.detail.isEmpty())
            label += QStringLiteral(" (%1)").arg(server.detail);
        auto* action = mcpMenu_->addAction(label);
        action->setObjectName(
            QStringLiteral("mcpServerAction_%1").arg(server.serverId));
        action->setCheckable(true);
        action->setChecked(server.enabled);
        action->setEnabled(server.available);
        action->setData(server.serverId);
        connect(action, &QAction::toggled, this,
                [this, server](bool enabled)
                {
                    QTimer::singleShot(
                        0, this,
                        [this, server, enabled]
                        {
                            if (server.builtIn)
                                emit builtInFilesystemMcpToggled(enabled);
                            else
                                emit externalMcpServerToggled(server.serverId,
                                                              enabled);
                        });
                });
    }

    mcpMenu_->addSeparator();
    auto* addAction = mcpMenu_->addAction(tr("Add MCP server..."));
    addAction->setObjectName(QStringLiteral("addMcpServerAction"));
    connect(addAction, &QAction::triggered, this,
            &ChatView::addMcpServerRequested);

    auto* removeMenu = mcpMenu_->addMenu(tr("Remove MCP server"));
    removeMenu->setObjectName(QStringLiteral("removeMcpServerMenu"));
    for (const auto& server : mcpServers_)
    {
        if (server.builtIn) continue;
        auto* removeAction = removeMenu->addAction(server.displayName);
        removeAction->setObjectName(
            QStringLiteral("removeMcpServerAction_%1").arg(server.serverId));
        connect(removeAction, &QAction::triggered, this, [this, server]
                { emit removeExternalMcpServerRequested(server.serverId); });
    }
    removeMenu->setEnabled(!removeMenu->actions().isEmpty());
}

bool ChatView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == ui_->modelPathLink && event->type() == QEvent::Resize)
        updateModelPresentation();
    if (watched == ui_->workspacePathLink && event->type() == QEvent::Resize)
        updateWorkspacePresentation();
    if (watched == ui_->workspacePathLink &&
        event->type() == QEvent::MouseButtonRelease)
    {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton &&
            ui_->workspacePathLink->isEnabled())
        {
            emit workspaceOpenRequested();
            return true;
        }
    }
    if (watched == ui_->workspacePathLink && event->type() == QEvent::KeyPress)
    {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (ui_->workspacePathLink->isEnabled() &&
            (keyEvent->key() == Qt::Key_Return ||
             keyEvent->key() == Qt::Key_Enter ||
             keyEvent->key() == Qt::Key_Space))
        {
            emit workspaceOpenRequested();
            return true;
        }
    }

    if (watched == ui_->promptEditor && event->type() == QEvent::KeyPress)
    {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        const auto isEnter = keyEvent->key() == Qt::Key_Return ||
                             keyEvent->key() == Qt::Key_Enter;
        if (isEnter && !(keyEvent->modifiers() & Qt::ShiftModifier))
        {
            if (ui_->primaryActionButton->isEnabled() && !primaryActionStops_)
                emit promptSubmitted();
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void ChatView::updateModelPresentation()
{
    const auto nativePath = QDir::toNativeSeparators(modelPath_);
    auto sourceText = nativePath.isEmpty() ? tr("Select model")
                                           : QFileInfo(modelPath_).fileName();
    if (sourceText.isEmpty()) sourceText = nativePath;
    const auto labelWidth = qBound(
        112,
        ui_->modelPathLink->fontMetrics().horizontalAdvance(sourceText) + 24,
        300);
    ui_->modelPathLink->setFixedWidth(labelWidth);
    const auto displayText = ui_->modelPathLink->fontMetrics().elidedText(
        sourceText, Qt::ElideMiddle, qMax(40, labelWidth - 4));
    ui_->modelPathLink->setEnabled(!modelPath_.isEmpty());
    ui_->modelPathLink->setText(
        modelPath_.isEmpty()
            ? displayText.toHtmlEscaped()
            : QStringLiteral("<a style=\"color:%1;text-decoration:none\" "
                             "href=\"open-model-location\">%2</a>")
                  .arg(modelBadgeTextColor(modelBadgeState_),
                       displayText.toHtmlEscaped()));
    const auto guideWidth = qBound(
        520,
        ui_->guideModelNameLabel->fontMetrics().horizontalAdvance(sourceText) +
            40,
        600);
    ui_->guideModelNameLabel->setFixedWidth(guideWidth);
    ui_->guideModelNameLabel->setText(
        ui_->guideModelNameLabel->fontMetrics().elidedText(
            sourceText, Qt::ElideMiddle, guideWidth - 36));

    const auto tooltip = nativePath.isEmpty()
                             ? modelInformation_
                             : tr("Open model folder: %1\n%2")
                                   .arg(nativePath, modelInformation_);
    ui_->modelPathLink->setToolTip(tooltip);
    ui_->guideModelNameLabel->setToolTip(tooltip);
}

void ChatView::updateWorkspacePresentation()
{
    if (workspacePath_.isEmpty())
    {
        ui_->workspacePathLink->clear();
        ui_->workspacePathLink->setAccessibleDescription({});
        ui_->workspacePathLink->setVisible(false);
        return;
    }

    const auto nativePath = QDir::toNativeSeparators(workspacePath_);
    auto folderName = QFileInfo(QDir::cleanPath(workspacePath_)).fileName();
    if (folderName.isEmpty()) folderName = nativePath;
    const auto labelWidth = qBound(
        56,
        ui_->workspacePathLink->fontMetrics().horizontalAdvance(folderName) +
            16,
        180);
    ui_->workspacePathLink->setFixedWidth(labelWidth);
    ui_->workspacePathLink->setText(
        ui_->workspacePathLink->fontMetrics().elidedText(
            folderName, Qt::ElideMiddle, labelWidth - 12));
    ui_->workspacePathLink->setEnabled(true);
    ui_->workspacePathLink->setVisible(true);
    ui_->workspacePathLink->setToolTip(
        tr("Open workspace folder: %1").arg(nativePath));
    ui_->workspacePathLink->setAccessibleDescription(
        tr("Workspace folder: %1").arg(nativePath));
}
}  // namespace qtllm::ui
