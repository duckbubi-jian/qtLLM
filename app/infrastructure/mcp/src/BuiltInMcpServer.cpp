#include "BuiltInMcpServer.hpp"

#include <QDir>
#include <QFileInfo>

#include <utility>

namespace qtllm::infrastructure::mcp
{
bool createBuiltInFilesystemServerConfig(const QString& applicationDirectory,
                                         const QString& applicationVersion,
                                         const QString& workspaceDirectory,
                                         McpServerConfig& config,
                                         QString& errorMessage)
{
    const QFileInfo workspaceInfo(workspaceDirectory);
    const auto workspacePath = workspaceInfo.canonicalFilePath();
    if (!workspaceInfo.isAbsolute() || !workspaceInfo.isDir() ||
        workspacePath.isEmpty())
    {
        errorMessage = QStringLiteral(
                           "Workspace is not an existing absolute "
                           "directory: %1")
                           .arg(workspaceDirectory);
        return false;
    }

    if (applicationVersion.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Application version is empty.");
        return false;
    }

#ifdef Q_OS_WIN
    const auto executableName = QStringLiteral("qtllm-mcp-filesystem.exe");
#else
    const auto executableName = QStringLiteral("qtllm-mcp-filesystem");
#endif
    const auto relativePath =
        QStringLiteral("mcp/builtin/qtllm.filesystem/%1/%2")
            .arg(applicationVersion, executableName);
    const QFileInfo executableInfo(
        QDir(applicationDirectory).filePath(relativePath));
    if (!executableInfo.isAbsolute() || !executableInfo.isFile())
    {
        errorMessage =
            QStringLiteral("Built-in filesystem MCP server was not found: %1")
                .arg(executableInfo.absoluteFilePath());
        return false;
    }
#ifndef Q_OS_WIN
    if (!executableInfo.isExecutable())
    {
        errorMessage = QStringLiteral(
                           "Built-in filesystem MCP server is not executable: "
                           "%1")
                           .arg(executableInfo.absoluteFilePath());
        return false;
    }
#endif

    McpServerConfig result;
    result.serverId = QStringLiteral("filesystem");
    result.program = executableInfo.absoluteFilePath();
    result.arguments = {QStringLiteral("--write-root"), workspacePath};
    result.workingDirectory = workspacePath;
    result.authorizedRoots = {workspacePath};
    result.toolAllowlist = {QStringLiteral("read_text_file"),
                            QStringLiteral("read_multiple_files"),
                            QStringLiteral("list_directory"),
                            QStringLiteral("directory_tree"),
                            QStringLiteral("search_files"),
                            QStringLiteral("get_file_info"),
                            QStringLiteral("list_allowed_directories"),
                            QStringLiteral("create_directory"),
                            QStringLiteral("write_file"),
                            QStringLiteral("edit_file"),
                            QStringLiteral("move_file"),
                            QStringLiteral("delete_file"),
                            QStringLiteral("delete_directory")};
    config = std::move(result);
    errorMessage.clear();
    return true;
}
}  // namespace qtllm::infrastructure::mcp
