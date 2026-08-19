#include "ToolPolicy.hpp"

#include <QSet>

namespace qtllm::infrastructure::mcp
{
ToolPolicyRule defaultToolPolicyRule(const QString& qualifiedToolName,
                                     const QJsonObject& annotations)
{
    const auto separator = qualifiedToolName.indexOf(QLatin1Char('.'));
    const auto serverId =
        separator > 0 ? qualifiedToolName.left(separator) : QString{};
    const auto toolName = separator > 0 ? qualifiedToolName.mid(separator + 1)
                                        : qualifiedToolName;
    if (serverId != QLatin1String("filesystem"))
    {
        if (annotations.value(QStringLiteral("destructiveHint")).toBool())
            return {ToolRisk::Destructive, true, false};
        if (annotations.value(QStringLiteral("readOnlyHint")).toBool())
            return {ToolRisk::ReadOnly, true, false};
        return {};
    }

    static const QSet<QString> readOnlyTools{
        QStringLiteral("read_file"),
        QStringLiteral("read_text_file"),
        QStringLiteral("read_media_file"),
        QStringLiteral("read_multiple_files"),
        QStringLiteral("list_directory"),
        QStringLiteral("list_directory_with_sizes"),
        QStringLiteral("directory_tree"),
        QStringLiteral("search_files"),
        QStringLiteral("get_file_info"),
        QStringLiteral("list_allowed_directories")};
    if (readOnlyTools.contains(toolName))
        return {ToolRisk::ReadOnly, true, true};
    if (toolName == QLatin1String("create_directory"))
        return {ToolRisk::CreatesData, true, false};
    if (toolName == QLatin1String("delete_file") ||
        toolName == QLatin1String("delete_directory"))
        return {ToolRisk::Destructive, true, false};
    return {ToolRisk::ModifiesData, true, false};
}

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
    if (iterator->alwaysAllow) return ToolDecision::Allow;
    return ToolDecision::RequireApproval;
}

ToolRisk ToolPolicy::risk(const QString& qualifiedToolName) const
{
    const auto iterator = rules_.constFind(qualifiedToolName);
    return iterator == rules_.constEnd() ? ToolRisk::ModifiesData
                                         : iterator->risk;
}

std::optional<ToolPolicyRule> ToolPolicy::rule(
    const QString& qualifiedToolName) const
{
    const auto iterator = rules_.constFind(qualifiedToolName);
    if (iterator == rules_.constEnd()) return std::nullopt;
    return *iterator;
}
}  // namespace qtllm::infrastructure::mcp
