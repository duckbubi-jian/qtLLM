#pragma once

#include <QString>
#include <QStringView>

namespace qtllm::chat
{
struct AssistantResponse
{
    QString reasoning;
    QString answer;
    bool hasReasoning = false;
    bool reasoningComplete = false;
};

AssistantResponse parseAssistantResponse(QStringView rawResponse);
QString assistantHistoryText(QStringView rawResponse);
}  // namespace qtllm::chat
