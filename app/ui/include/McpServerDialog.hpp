#pragma once

#include "McpServerProcess.hpp"

#include <QDialog>
#include <QStringList>

#include <memory>

namespace Ui
{
class McpServerDialog;
}

namespace qtllm::ui
{
class McpServerDialog final : public QDialog
{
    Q_OBJECT

   public:
    explicit McpServerDialog(QStringList existingServerIds,
                             QWidget* parent = nullptr);
    ~McpServerDialog() override;

    [[nodiscard]] infrastructure::mcp::McpServerConfig configuration() const;

   protected:
    void accept() override;

   private:
    bool configureFastMcpPackage();
    bool configureCustomServer();
    bool validateServerId(const QString& serverId);
    void browseFastMcpFolder();
    void browseProgram();
    void browseWorkingDirectory();
    void showValidationError(const QString& message);

    std::unique_ptr<Ui::McpServerDialog> ui_;
    QStringList existingServerIds_;
    infrastructure::mcp::McpServerConfig configuration_;
};
}  // namespace qtllm::ui
