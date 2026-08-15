#include "ChatView.hpp"

#include "AutoHideTabWidget.hpp"
#include "ui_ChatView.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
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
    ui_->actionRow->setStretch(1, 1);
    ui_->clearConversationButton->setIcon(
        style()->standardIcon(QStyle::SP_DialogResetButton));
    ui_->modelReloadButton->setIcon(
        style()->standardIcon(QStyle::SP_BrowserReload));
    ui_->workspaceBrowseButton->setIcon(
        style()->standardIcon(QStyle::SP_DirOpenIcon));
    ui_->guideSelectModelButton->setIcon(
        style()->standardIcon(QStyle::SP_DirOpenIcon));
    ui_->guideLoadModelButton->setIcon(
        style()->standardIcon(QStyle::SP_MediaPlay));

    ui_->workspacePathLink->setSizePolicy(QSizePolicy::Ignored,
                                          QSizePolicy::Preferred);
    ui_->modelPathLink->setSizePolicy(QSizePolicy::Fixed,
                                      QSizePolicy::Preferred);
    ui_->workspacePathLink->installEventFilter(this);
    ui_->modelPathLink->installEventFilter(this);
    ui_->promptEditor->installEventFilter(this);

    connect(ui_->modelReloadButton, &QToolButton::clicked, this,
            &ChatView::modelFolderRequested);
    connect(ui_->guideSelectModelButton, &QPushButton::clicked, this,
            &ChatView::modelFolderRequested);
    connect(ui_->modelPathLink, &QLabel::linkActivated, this,
            &ChatView::modelLocationRequested);
    connect(ui_->guideLoadModelButton, &QPushButton::clicked, this,
            &ChatView::modelLoadRequested);
    connect(ui_->workspaceBrowseButton, &QToolButton::clicked, this,
            &ChatView::workspaceFolderRequested);
    connect(ui_->workspacePathLink, &QLabel::linkActivated, this,
            &ChatView::workspaceOpenRequested);
    connect(ui_->primaryActionButton, &QPushButton::clicked, this,
            &ChatView::primaryActionRequested);
    connect(ui_->clearConversationButton, &QToolButton::clicked, this,
            &ChatView::clearConversationRequested);

    setModelPresentation({}, tr("Model not checked"), ModelBadgeState::Neutral);
    setWorkspacePresentation({});
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
    ui_->modelReloadButton->setEnabled(selectionEnabled);
    ui_->guideSelectModelButton->setEnabled(selectionEnabled);
    ui_->guideLoadModelButton->setEnabled(loadEnabled);
}

void ChatView::setWorkspaceControlsEnabled(bool enabled)
{
    ui_->workspaceBrowseButton->setEnabled(enabled);
    ui_->workspacePathLink->setEnabled(enabled);
}

void ChatView::setPromptEnabled(bool enabled)
{
    ui_->promptEditor->setEnabled(enabled);
}

void ChatView::setClearEnabled(bool enabled)
{
    ui_->clearConversationButton->setEnabled(enabled);
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

bool ChatView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == ui_->modelPathLink && event->type() == QEvent::Resize)
        updateModelPresentation();
    if (watched == ui_->workspacePathLink && event->type() == QEvent::Resize)
        updateWorkspacePresentation();

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
    ui_->guideModelNameLabel->setText(sourceText);

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
        return;
    }

    const auto nativePath = QDir::toNativeSeparators(workspacePath_);
    const auto availableWidth = qMax(80, ui_->workspacePathLink->width() - 4);
    const auto displayPath = ui_->workspacePathLink->fontMetrics().elidedText(
        nativePath, Qt::ElideMiddle, availableWidth);
    const auto href = QUrl::fromLocalFile(workspacePath_)
                          .toString(QUrl::FullyEncoded)
                          .toHtmlEscaped();
    ui_->workspacePathLink->setText(
        QStringLiteral("<a style=\"color:#2563eb;text-decoration:none\" "
                       "href=\"%1\">%2</a>")
            .arg(href, displayPath.toHtmlEscaped()));
    ui_->workspacePathLink->setToolTip(
        tr("Open workspace folder: %1").arg(nativePath));
}
}  // namespace qtllm::ui
