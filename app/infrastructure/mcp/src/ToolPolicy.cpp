#include "ToolPolicy.hpp"

namespace qtllm::infrastructure::mcp
{
void ToolPolicy::setRule(const QString& qualifiedToolName, ToolPolicyRule rule)
{
    if (!qualifiedToolName.trimmed().isEmpty())
        rules_.insert(qualifiedToolName, rule);
}

void ToolPolicy::removeRule(const QString& qualifiedToolName)
{
    rules_.remove(qualifiedToolName);
}

void ToolPolicy::clear()
{
    rules_.clear();
}

ToolDecision ToolPolicy::evaluate(const QString& qualifiedToolName) const
{
    const auto iterator = rules_.constFind(qualifiedToolName);
    if (iterator == rules_.constEnd()) return ToolDecision::RequireApproval;
    if (!iterator->enabled) return ToolDecision::Deny;
    if (iterator->risk == ToolRisk::ReadOnly && iterator->allowAutomaticRead)
        return ToolDecision::Allow;
    return ToolDecision::RequireApproval;
}

ToolRisk ToolPolicy::risk(const QString& qualifiedToolName) const
{
    const auto iterator = rules_.constFind(qualifiedToolName);
    return iterator == rules_.constEnd() ? ToolRisk::ModifiesData
                                         : iterator->risk;
}
}  // namespace qtllm::infrastructure::mcp
