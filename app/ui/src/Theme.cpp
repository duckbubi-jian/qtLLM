#include "Theme.hpp"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QStyle>
#include <QStyleFactory>

namespace qtllm::ui
{
void applyApplicationTheme(QApplication& application)
{
    if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        application.setStyle(fusion);

    QFont font = application.font();
    const auto families = QFontDatabase::families();
    if (families.contains(QStringLiteral("Segoe UI")))
        font.setFamily(QStringLiteral("Segoe UI"));
    font.setPointSize(10);
    application.setFont(font);

    application.setStyleSheet(QStringLiteral(R"(
QWidget {
    color: #202124;
}
QMainWindow, QWidget#centralView {
    background: #ffffff;
}
QStackedWidget#contentStack,
QWidget#modelGuidePage,
QWidget#transcriptPage {
    background: #ffffff;
}
QPushButton {
    min-height: 34px;
    padding: 0 13px;
    background: #ffffff;
    border: 1px solid #d7dbe0;
    border-radius: 6px;
}
QPushButton:hover {
    background: #f0f2f5;
    border-color: #b8bec7;
}
QPushButton:pressed {
    background: #e7eaee;
}
QPushButton:disabled {
    color: #9aa0a6;
    background: #f1f2f4;
    border-color: #e0e3e7;
}
QPushButton#primaryActionButton {
    min-width: 36px;
    max-width: 36px;
    min-height: 36px;
    max-height: 36px;
    padding: 0;
    color: #ffffff;
    background: #3b82f6;
    border-color: #3b82f6;
}
QPushButton#primaryActionButton:hover {
    background: #2563eb;
    border-color: #2563eb;
}
QPushButton#primaryActionButton[stopMode="true"] {
    background: #b42318;
    border-color: #b42318;
}
QPushButton#primaryActionButton[stopMode="true"]:hover {
    background: #922018;
    border-color: #922018;
}
QPushButton#primaryActionButton:disabled {
    color: #ffffff;
    background: #cbd5e1;
    border-color: #cbd5e1;
}
QTabWidget#transcriptTabs::pane {
    background: transparent;
    border: none;
}
QTabBar::tab {
    min-width: 92px;
    min-height: 34px;
    padding: 0 12px;
    color: #5f6368;
    background: transparent;
    border: none;
    border-bottom: 2px solid transparent;
}
QTabBar::tab:hover {
    color: #202124;
    background: #eef1f4;
}
QTabBar::tab:selected {
    color: #1d4ed8;
    border-bottom-color: #2563eb;
    font-weight: 600;
}
QScrollArea#conversationScroll,
QWidget#conversationContent,
QPlainTextEdit#activityLog {
    background: #ffffff;
    border: none;
}
QWidget#promptComposer {
    background: #fbfcfe;
    border: 1px solid #dde3ea;
    border-radius: 8px;
}
QWidget#promptComposer[agentMode="true"] {
    background: #f8fbff;
    border-color: #bfdbfe;
}
QToolButton#mcpMenuButton {
    min-width: 28px;
    max-width: 28px;
    min-height: 28px;
    max-height: 28px;
    padding: 0;
    color: #334155;
    background: #ffffff;
    border: 1px solid #d7dbe0;
    border-radius: 14px;
    font-size: 15pt;
}
QToolButton#mcpMenuButton:hover {
    color: #1d4ed8;
    background: #eff6ff;
    border-color: #93c5fd;
}
QToolButton#mcpMenuButton:disabled {
    color: #94a3b8;
    background: #f1f5f9;
    border-color: #e2e8f0;
}
QDialog#mcpControlPanel {
    background: #ffffff;
}
QDialog#mcpControlPanel QTreeWidget,
QDialog#mcpControlPanel QPlainTextEdit,
QDialog#mcpControlPanel QLineEdit {
    color: #202124;
    background: #ffffff;
    border: 1px solid #d7dbe0;
    border-radius: 4px;
    selection-color: #172033;
    selection-background-color: #dbeafe;
}
QDialog#mcpControlPanel QTreeWidget::item {
    min-height: 28px;
}
QDialog#mcpControlPanel QHeaderView::section {
    min-height: 28px;
    padding: 0 7px;
    color: #475569;
    background: #f5f6f8;
    border: none;
    border-right: 1px solid #e2e5e9;
    border-bottom: 1px solid #d7dbe0;
}
QDialog#mcpControlPanel QTabWidget::pane {
    border: 1px solid #d7dbe0;
}
QLabel#mcpServerName {
    color: #202124;
    font-size: 13pt;
    font-weight: 600;
}
QLabel#mcpServerState {
    min-height: 24px;
    padding: 0 8px;
    color: #475569;
    background: #f1f5f9;
    border: 1px solid #d7dbe0;
    border-radius: 4px;
}
QLabel#mcpServerState[serverState="ready"] {
    color: #166534;
    background: #ecfdf3;
    border-color: #86d7a5;
}
QLabel#mcpServerState[serverState="degraded"] {
    color: #854d0e;
    background: #fffbeb;
    border-color: #e5b94f;
}
QLabel#mcpServerState[serverState="failed"] {
    color: #991b1b;
    background: #fff1f2;
    border-color: #e69a9a;
}
QLabel#mcpServerState[serverState="starting"],
QLabel#mcpServerState[serverState="initializing"],
QLabel#mcpServerState[serverState="stopping"] {
    color: #1d4ed8;
    background: #eff6ff;
    border-color: #93c5fd;
}
QDialog#mcpControlPanel QToolButton {
    min-height: 30px;
    padding: 0 9px;
    color: #334155;
    background: #ffffff;
    border: 1px solid #d7dbe0;
    border-radius: 5px;
}
QDialog#mcpControlPanel QToolButton:hover {
    background: #eef1f4;
}
QDialog#mcpControlPanel QToolButton:disabled {
    color: #9aa0a6;
    background: #f5f6f8;
    border-color: #e2e5e9;
}
QPlainTextEdit#mcpDiagnostics,
QPlainTextEdit#mcpInstructions {
    padding: 6px 8px;
    font-family: "Cascadia Mono", "Consolas", monospace;
}
QLabel#mcpLastError {
    color: #991b1b;
}
QLabel#workspacePathLink {
    min-height: 24px;
    max-height: 24px;
    color: #854d0e;
    padding: 0 4px 0 7px;
    background: transparent;
    border: none;
    border-left: 5px solid #eab308;
    border-radius: 0;
}
QLabel#workspacePathLink:hover {
    background: #fffbeb;
    border-left-color: #f59e0b;
}
QLabel#workspacePathLink:disabled {
    color: #a8a29e;
    background: transparent;
    border-left-color: #d6d3d1;
}
QLabel#modelPathLink {
    min-height: 28px;
    max-height: 28px;
    color: #475569;
    padding: 0 10px;
    background: #f1f5f9;
    border: 1px solid #e2e8f0;
    border-radius: 6px;
}
QLabel#modelPathLink:hover {
    background: #e8eef5;
    border-color: #cbd5e1;
}
QLabel#modelPathLink[modelState="checking"],
QLabel#guideModelNameLabel[modelState="checking"] {
    color: #1d4ed8;
    background: #eff6ff;
    border-color: #bfdbfe;
}
QLabel#modelPathLink[modelState="verified"],
QLabel#guideModelNameLabel[modelState="verified"] {
    color: #166534;
    background: #ecfdf3;
    border-color: #bbf7d0;
}
QLabel#modelPathLink[modelState="unverified"],
QLabel#guideModelNameLabel[modelState="unverified"] {
    color: #9f1239;
    background: #fff1f2;
    border-color: #fecdd3;
}
QLabel#modelPathLink[modelState="invalid"],
QLabel#guideModelNameLabel[modelState="invalid"] {
    color: #991b1b;
    background: #fee2e2;
    border-color: #fca5a5;
}
QLabel#guideTitleLabel {
    color: #202124;
    font-size: 17pt;
    font-weight: 600;
}
QLabel#guideStatusLabel {
    color: #5f6368;
    padding: 8px 0 0 0;
}
QLabel#guideModelNameLabel {
    color: #334155;
    padding: 10px 18px;
    background: #f1f3f5;
    border: 1px solid #dfe3e8;
    border-radius: 5px;
    font-family: "Cascadia Mono", "Consolas", monospace;
}
QLabel#guideComputeSummaryLabel {
    color: #64748b;
    padding: 0 2px;
}
QToolButton#guideComputeSettingsButton {
    border: 1px solid transparent;
    border-radius: 5px;
}
QToolButton#guideComputeSettingsButton:hover {
    background: #e8eef5;
    border-color: #cbd5e1;
}
QPushButton#guideLoadModelButton {
    min-width: 180px;
    min-height: 42px;
    padding: 0 20px;
    color: #ffffff;
    background: #2563eb;
    border-color: #2563eb;
    font-weight: 600;
}
QPushButton#guideLoadModelButton:hover {
    background: #1d4ed8;
    border-color: #1d4ed8;
}
QPushButton#guideLoadModelButton:disabled {
    color: #ffffff;
    background: #aeb4bd;
    border-color: #aeb4bd;
}
QPushButton#guideSelectModelButton {
    min-width: 180px;
    min-height: 42px;
    padding: 0 20px;
}
QPlainTextEdit#promptEditor {
    padding: 8px 10px 2px 10px;
    background: transparent;
    border: none;
    selection-background-color: #2563eb;
}
QWidget#userMessage,
QWidget#assistantMessage {
    background: transparent;
}
QLabel#userAvatar,
QLabel#assistantAvatar {
    min-width: 36px;
    max-width: 36px;
    min-height: 36px;
    max-height: 36px;
    padding: 0;
    color: #ffffff;
    border: none;
    border-radius: 18px;
    font-size: 8pt;
    font-weight: 600;
}
QLabel#userAvatar {
    background: #3b82f6;
}
QLabel#assistantAvatar {
    background: #14a38b;
}
QWidget#userMessageContent,
QWidget#assistantMessageContent {
    background: transparent;
}
QTextBrowser#userMessageBody {
    color: #172033;
    background: #e7f0ff;
    border: none;
    border-radius: 8px;
}
QTextBrowser#assistantMessageBody {
    color: #202124;
    background: #f1f3f5;
    border: none;
    border-radius: 8px;
}
QWidget#agentProgressCard {
    margin: 6px 12px;
    color: #202124;
    background: #f7f9fc;
    border: 1px solid #d8e0ea;
    border-radius: 5px;
}
QWidget#agentProgressCard[progressState="terminal"] {
    background: #fafbfc;
    border-color: #e1e5ea;
}
QToolButton#agentProgressStateToggle {
    min-height: 24px;
    padding: 0 8px 0 4px;
    color: #30445f;
    background: #eaf2fc;
    border: none;
    border-radius: 4px;
    font-weight: 600;
    text-align: left;
}
QToolButton#agentProgressStateToggle:hover {
    background: #e1ecf9;
}
QLabel#agentProgressElapsed {
    min-height: 24px;
    padding: 0 6px;
    color: #526174;
    background: #e9eef5;
    border: none;
    border-radius: 4px;
    font-family: "Cascadia Mono", "Consolas", monospace;
}
QWidget#agentProgressPlan {
    background: #fbfcfe;
    border-top: 1px solid #e2e8f0;
}
QLabel#agentProgressFinish {
    color: #991b1b;
}
QWidget#agentProgressStep {
    background: transparent;
    border-bottom: 1px solid #edf1f5;
}
QLabel#agentProgressStepDescription {
    color: #475569;
}
QWidget#toolApprovalCard {
    margin: 5px 12px;
    background: #f8fafc;
    border: 1px solid #d9e1ea;
    border-radius: 5px;
}
QWidget#toolApprovalCard[riskLevel="read"] {
    background: #f7f9fc;
    border-color: #cbd8e8;
}
QWidget#toolApprovalCard[riskLevel="destructive"] {
    background: #fff5f4;
    border-color: #d6655a;
}
QToolButton#toolApprovalToggle {
    min-height: 24px;
    padding: 0 4px;
    color: #202124;
    background: transparent;
    border: none;
    font-weight: 600;
}
QToolButton#toolApprovalToggle:hover {
    background: #edf1f6;
    border-radius: 4px;
}
QLabel#toolApprovalDescription,
QLabel#toolApprovalArgumentLabel {
    color: #5f6368;
}
QLabel#toolApprovalArgumentLabel {
    font-size: 9pt;
    font-weight: 600;
}
QToolButton#toolApprovalArgumentsToggle {
    min-height: 24px;
    padding: 0 4px;
    color: #475569;
    background: transparent;
    border: none;
    font-weight: 600;
}
QToolButton#toolApprovalArgumentsToggle:hover {
    color: #1f2937;
    background: #f1f5f9;
    border-radius: 4px;
}
QLabel#toolApprovalName {
    color: #334155;
    font-family: "Cascadia Mono", "Consolas", monospace;
}
QPlainTextEdit#toolApprovalArguments {
    padding: 6px 8px;
    color: #334155;
    background: #ffffff;
    border: 1px solid #d7dbe0;
    border-radius: 4px;
    selection-background-color: #2563eb;
}
QPushButton#allowToolButton {
    color: #ffffff;
    background: #2563eb;
    border-color: #2563eb;
    font-weight: 600;
}
QPushButton#allowToolButton:hover {
    background: #1d4ed8;
    border-color: #1d4ed8;
}
QPushButton#rejectToolButton {
    color: #b42318;
}
QLabel#toolApprovalStatus {
    color: #b42318;
    font-weight: 600;
}
QLabel#toolApprovalStatus[approved="true"] {
    color: #18794e;
}
QToolButton#reasoningToggle {
    min-height: 26px;
    padding: 0 7px;
    color: #5f6368;
    background: transparent;
    border: none;
    border-radius: 4px;
}
QToolButton#reasoningToggle:hover {
    color: #202124;
    background: #eef1f4;
}
QTextBrowser#reasoningBody {
    color: #5f6368;
    background: #f6f7f9;
    border: none;
    border-left: 3px solid #c4c9d1;
}
QWidget#codeActions QLabel {
    color: #5f6368;
}
QToolButton#messageToolButton {
    min-height: 28px;
    padding: 0 8px;
    color: #334155;
    background: #ffffff;
    border: 1px solid #d7dbe0;
    border-radius: 5px;
}
QToolButton#messageToolButton:hover {
    background: #eef1f4;
}
QLabel#statusLabel {
    color: #5f6368;
    background: #f5f6f8;
    padding: 3px 2px 0 2px;
    border-top: 1px solid #dfe3e8;
}
QScrollBar:vertical {
    width: 10px;
    background: transparent;
    margin: 2px;
}
QScrollBar::handle:vertical {
    min-height: 32px;
    background: #c7cbd1;
    border-radius: 4px;
}
QScrollBar::handle:vertical:hover {
    background: #aeb4bd;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical,
QScrollBar::add-page:vertical,
QScrollBar::sub-page:vertical {
    height: 0;
    background: transparent;
}
)"));
}
}  // namespace qtllm::ui
