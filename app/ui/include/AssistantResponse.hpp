#pragma once

#include <QString>
#include <QStringView>

namespace qtllm::ui
{
struct AssistantResponse
{
    QString reasoning;
    QString answer;
    bool hasReasoning = false;
    bool reasoningComplete = false;
};

AssistantResponse parseAssistantResponse(QStringView rawResponse);
}  // namespace qtllm::ui
