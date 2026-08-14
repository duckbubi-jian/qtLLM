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
    background: #f5f6f8;
}
QWidget#modelBar {
    background: transparent;
}
QLineEdit#modelPathEdit {
    min-height: 36px;
    padding: 0 10px;
    background: #ffffff;
    border: 1px solid #d7dbe0;
    border-radius: 6px;
    selection-background-color: #2563eb;
}
QLineEdit#modelPathEdit:focus {
    border-color: #2563eb;
}
QLabel#modelInfoLabel {
    color: #5f6368;
    padding: 0 2px 2px 2px;
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
    color: #ffffff;
    background: #2563eb;
    border-color: #2563eb;
    font-weight: 600;
}
QPushButton#primaryActionButton:hover {
    background: #1d4ed8;
    border-color: #1d4ed8;
}
QPushButton#stopButton {
    color: #b42318;
}
QTabWidget#transcriptTabs::pane {
    background: #ffffff;
    border: 1px solid #dfe3e8;
    border-radius: 6px;
    top: -1px;
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
QPlainTextEdit#rawTranscript {
    background: #ffffff;
    border: none;
}
QWidget#promptComposer {
    background: #ffffff;
    border: 1px solid #d7dbe0;
    border-radius: 8px;
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
QLabel#userRoleLabel,
QLabel#assistantRoleLabel {
    color: #5f6368;
    font-size: 9pt;
    font-weight: 600;
}
QTextBrowser#userMessageBody {
    color: #172033;
    background: #e9f1ff;
    border: 1px solid #d6e4ff;
    border-radius: 8px;
}
QTextBrowser#assistantMessageBody {
    color: #202124;
    background: transparent;
    border: none;
}
QWidget#toolApprovalCard {
    margin: 8px 12px;
    background: #fffaf0;
    border: 1px solid #e5b94f;
    border-radius: 6px;
}
QWidget#toolApprovalCard[riskLevel="read"] {
    background: #f4f8ff;
    border-color: #8bb4e8;
}
QWidget#toolApprovalCard[riskLevel="destructive"] {
    background: #fff5f4;
    border-color: #d6655a;
}
QLabel#toolApprovalHeading {
    color: #202124;
    font-weight: 600;
}
QLabel#toolApprovalDescription,
QLabel#toolApprovalArgumentLabel {
    color: #5f6368;
}
QLabel#toolApprovalArgumentLabel {
    font-size: 9pt;
    font-weight: 600;
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
QStatusBar {
    color: #5f6368;
    background: #f5f6f8;
    border-top: 1px solid #dfe3e8;
}
QStatusBar::item {
    border: none;
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
