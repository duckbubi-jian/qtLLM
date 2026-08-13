#include "AssistantResponse.hpp"

namespace qtllm::ui
{
namespace
{
QString combineAnswerParts(const QString& before, const QString& after)
{
    const auto trimmedBefore = before.trimmed();
    const auto trimmedAfter = after.trimmed();
    if (trimmedBefore.isEmpty()) return trimmedAfter;
    if (trimmedAfter.isEmpty()) return trimmedBefore;
    return trimmedBefore + QStringLiteral("\n\n") + trimmedAfter;
}
}  // namespace

AssistantResponse parseAssistantResponse(QStringView rawResponse)
{
    const auto source = rawResponse.toString();
    const auto openTag = QStringLiteral("<think>");
    const auto closeTag = QStringLiteral("</think>");
    const auto openIndex = source.indexOf(openTag, 0, Qt::CaseInsensitive);
    const auto closeIndex = source.indexOf(closeTag, 0, Qt::CaseInsensitive);

    if (openIndex >= 0)
    {
        const auto reasoningStart = openIndex + openTag.size();
        const auto matchingClose =
            source.indexOf(closeTag, reasoningStart, Qt::CaseInsensitive);
        if (matchingClose >= 0)
        {
            return {
                source.mid(reasoningStart, matchingClose - reasoningStart)
                    .trimmed(),
                combineAnswerParts(source.left(openIndex),
                                   source.mid(matchingClose + closeTag.size())),
                true, true};
        }

        return {source.mid(reasoningStart).trimmed(),
                source.left(openIndex).trimmed(), true, false};
    }

    if (closeIndex >= 0)
    {
        return {source.left(closeIndex).trimmed(),
                source.mid(closeIndex + closeTag.size()).trimmed(), true, true};
    }

    return {{}, source.trimmed(), false, false};
}

QString assistantHistoryText(QStringView rawResponse)
{
    const auto response = parseAssistantResponse(rawResponse);
    if (response.hasReasoning && !response.reasoningComplete) return {};
    return response.answer.trimmed();
}
}  // namespace qtllm::ui
