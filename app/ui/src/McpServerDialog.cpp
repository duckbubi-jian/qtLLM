#include "McpServerDialog.hpp"

#include "ui_McpServerDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QStyle>
#include <QTabWidget>
#include <QToolButton>

#include <utility>

namespace qtllm::ui
{
namespace
{
QList<QFileInfo> packageExecutables(const QDir& directory)
{
    QList<QFileInfo> candidates;
    const auto files =
        directory.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& file : files)
    {
#ifdef Q_OS_WIN
        if (file.suffix().compare(QLatin1String("exe"), Qt::CaseInsensitive) ==
            0)
#else
        if (file.isExecutable())
#endif
            candidates.append(file);
    }
    return candidates;
}

QFileInfo preferredPackageExecutable(const QDir& directory,
                                     const QList<QFileInfo>& candidates)
{
    if (candidates.size() == 1) return candidates.constFirst();

    QList<QFileInfo> matchingFolderName;
    for (const auto& candidate : candidates)
    {
        if (candidate.completeBaseName().compare(directory.dirName(),
                                                 Qt::CaseInsensitive) == 0)
            matchingFolderName.append(candidate);
    }
    if (matchingFolderName.size() == 1) return matchingFolderName.constFirst();

    QList<QFileInfo> matchingMcpName;
    for (const auto& candidate : candidates)
    {
        if (candidate.completeBaseName().contains(QLatin1String("mcp"),
                                                  Qt::CaseInsensitive))
            matchingMcpName.append(candidate);
    }
    return matchingMcpName.size() == 1 ? matchingMcpName.constFirst()
                                       : QFileInfo{};
}

QString serverIdFromProgram(const QFileInfo& program)
{
    auto serverId = program.completeBaseName().toLower();
    serverId.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")),
                     QStringLiteral("-"));
    serverId.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    return serverId;
}
}  // namespace

McpServerDialog::McpServerDialog(QStringList existingServerIds, QWidget* parent)
    : QDialog(parent),
      ui_(std::make_unique<Ui::McpServerDialog>()),
      existingServerIds_(std::move(existingServerIds))
{
    ui_->setupUi(this);
    setModal(true);
    ui_->fastMcpFolderBrowseButton->setIcon(
        style()->standardIcon(QStyle::SP_DirOpenIcon));
    ui_->programBrowseButton->setIcon(
        style()->standardIcon(QStyle::SP_DialogOpenButton));
    ui_->workingDirectoryBrowseButton->setIcon(
        style()->standardIcon(QStyle::SP_DirOpenIcon));
    connect(ui_->fastMcpFolderBrowseButton, &QToolButton::clicked, this,
            &McpServerDialog::browseFastMcpFolder);
    connect(ui_->programBrowseButton, &QToolButton::clicked, this,
            &McpServerDialog::browseProgram);
    connect(ui_->workingDirectoryBrowseButton, &QToolButton::clicked, this,
            &McpServerDialog::browseWorkingDirectory);
    connect(ui_->buttonBox, &QDialogButtonBox::accepted, this,
            &McpServerDialog::accept);
    connect(ui_->buttonBox, &QDialogButtonBox::rejected, this,
            &McpServerDialog::reject);
    connect(ui_->configurationTabs, &QTabWidget::currentChanged, this,
            [this] { ui_->errorLabel->hide(); });
}

McpServerDialog::~McpServerDialog() = default;

infrastructure::mcp::McpServerConfig McpServerDialog::configuration() const
{
    return configuration_;
}

void McpServerDialog::accept()
{
    configuration_ = {};
    const auto configured =
        ui_->configurationTabs->currentWidget() == ui_->fastMcpTab
            ? configureFastMcpPackage()
            : configureCustomServer();
    if (!configured) return;

    configuration_.enabled = true;
    QDialog::accept();
}

bool McpServerDialog::configureFastMcpPackage()
{
    const QFileInfo folderInfo(ui_->fastMcpFolderEdit->text().trimmed());
    const auto folderPath = folderInfo.canonicalFilePath();
    if (!folderInfo.isAbsolute() || !folderInfo.isDir() || folderPath.isEmpty())
    {
        showValidationError(tr("Select an existing FastMCP package folder."));
        return false;
    }

    const QDir packageDirectory(folderPath);
    const auto candidates = packageExecutables(packageDirectory);
    if (candidates.isEmpty())
    {
        showValidationError(
            tr("The FastMCP package folder contains no executable."));
        return false;
    }
    const auto program =
        preferredPackageExecutable(packageDirectory, candidates);
    if (!program.isFile())
    {
        showValidationError(
            tr("The FastMCP package folder contains multiple "
               "possible server executables."));
        return false;
    }

    const auto serverId = serverIdFromProgram(program);
    if (!validateServerId(serverId)) return false;

    configuration_.serverId = serverId;
    configuration_.program = program.canonicalFilePath();
    configuration_.workingDirectory = folderPath;
    return true;
}

bool McpServerDialog::configureCustomServer()
{
    const auto serverId = ui_->serverIdEdit->text().trimmed();
    if (!validateServerId(serverId)) return false;

    const QFileInfo programInfo(ui_->programEdit->text().trimmed());
    if (!programInfo.isAbsolute() || !programInfo.isFile())
    {
        showValidationError(tr("Select an existing MCP server program."));
        return false;
    }

    const auto workingDirectoryText =
        ui_->workingDirectoryEdit->text().trimmed();
    QString workingDirectory;
    if (!workingDirectoryText.isEmpty())
    {
        const QFileInfo workingDirectoryInfo(workingDirectoryText);
        workingDirectory = workingDirectoryInfo.canonicalFilePath();
        if (!workingDirectoryInfo.isAbsolute() ||
            !workingDirectoryInfo.isDir() || workingDirectory.isEmpty())
        {
            showValidationError(tr("Select an existing working directory."));
            return false;
        }
    }

    configuration_.serverId = serverId;
    configuration_.program = programInfo.absoluteFilePath();
    configuration_.arguments =
        QProcess::splitCommand(ui_->argumentsEdit->text().trimmed());
    configuration_.workingDirectory = workingDirectory;
    return true;
}

bool McpServerDialog::validateServerId(const QString& serverId)
{
    static const QRegularExpression validServerId(
        QStringLiteral("^[A-Za-z0-9_-]+$"));
    if (!validServerId.match(serverId).hasMatch())
    {
        showValidationError(
            tr("Server ID may contain letters, numbers, '-' and '_' only."));
        return false;
    }
    if (existingServerIds_.contains(serverId, Qt::CaseInsensitive))
    {
        showValidationError(tr("An MCP server with this ID already exists."));
        return false;
    }
    return true;
}

void McpServerDialog::browseFastMcpFolder()
{
    const QFileInfo current(ui_->fastMcpFolderEdit->text().trimmed());
    const auto initialDirectory =
        current.isDir() ? current.absoluteFilePath() : QDir::homePath();
    const auto selected = QFileDialog::getExistingDirectory(
        this, tr("Select FastMCP package folder"), initialDirectory,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) return;

    ui_->fastMcpFolderEdit->setText(QFileInfo(selected).absoluteFilePath());
}

void McpServerDialog::browseProgram()
{
    const QFileInfo current(ui_->programEdit->text().trimmed());
    const auto initialDirectory =
        current.isFile() ? current.absolutePath() : QDir::homePath();
    const auto selected = QFileDialog::getOpenFileName(
        this, tr("Select MCP server program"), initialDirectory);
    if (selected.isEmpty()) return;

    const QFileInfo selectedInfo(selected);
    ui_->programEdit->setText(selectedInfo.absoluteFilePath());
    if (ui_->serverIdEdit->text().trimmed().isEmpty())
    {
        auto serverId = selectedInfo.completeBaseName();
        serverId.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")),
                         QStringLiteral("-"));
        ui_->serverIdEdit->setText(serverId);
    }
}

void McpServerDialog::browseWorkingDirectory()
{
    const QFileInfo current(ui_->workingDirectoryEdit->text().trimmed());
    const auto initialDirectory =
        current.isDir() ? current.absoluteFilePath() : QDir::homePath();
    const auto selected = QFileDialog::getExistingDirectory(
        this, tr("Select MCP working directory"), initialDirectory,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!selected.isEmpty())
        ui_->workingDirectoryEdit->setText(
            QFileInfo(selected).absoluteFilePath());
}

void McpServerDialog::showValidationError(const QString& message)
{
    ui_->errorLabel->setText(message);
    ui_->errorLabel->setVisible(true);
}
}  // namespace qtllm::ui
