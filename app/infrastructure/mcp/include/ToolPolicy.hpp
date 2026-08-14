#pragma once

#include <QHash>
#include <QMetaType>
#include <QString>

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
    bool allowAutomaticRead = false;
};

class ToolPolicy final
{
   public:
    void setRule(const QString& qualifiedToolName, ToolPolicyRule rule);
    void removeRule(const QString& qualifiedToolName);
    void clear();

    [[nodiscard]] ToolDecision evaluate(const QString& qualifiedToolName) const;
    [[nodiscard]] ToolRisk risk(const QString& qualifiedToolName) const;

   private:
    QHash<QString, ToolPolicyRule> rules_;
};
}  // namespace qtllm::infrastructure::mcp

Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::ToolRisk)
Q_DECLARE_METATYPE(qtllm::infrastructure::mcp::ToolDecision)
