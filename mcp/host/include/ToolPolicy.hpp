#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMetaType>
#include <QString>

#include <optional>

namespace qtllm::infrastructure::mcp
{
enum class ToolRisk
{
    ReadOnly,
    CreatesData,
    ModifiesData,
    Destructive
};

enum class ToolDecision
{
    Allow,
    RequireApproval,
    Deny
};

struct ToolPolicyRule
{
    ToolRisk risk = ToolRisk::ModifiesData;
    bool enabled = true;
    bool alwaysAllow = false;
};

[[nodiscard]] ToolPolicyRule defaultToolPolicyRule(
    const QString& qualifiedToolName, const QJsonObject& annotations = {});

class ToolPolicy final
{
   public:
    void setRule(const QString& qualifiedToolName, ToolPolicyRule rule);
    void removeRule(const QString& qualifiedToolName);
    void clear();

    [[nodiscard]] ToolDecision evaluate(const QString& qualifiedToolName) const;
    [[nodiscard]] ToolRisk risk(const QString& qualifiedToolName) const;
    [[nodiscard]] std::optional<ToolPolicyRule> rule(
        const QString& qualifiedToolName) const;

   private:
    QHash<QString, ToolPolicyRule> rules_;
};
}  // namespace qtllm::infrastructure::mcp

Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::ToolRisk)
Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::ToolDecision)
