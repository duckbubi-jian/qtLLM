#pragma once

#include "ChatMessage.hpp"
#include "ComputeDevice.hpp"
#include "InferenceDefaults.hpp"
#include "ResponseMode.hpp"

#include <QList>
#include <QString>

#include <cstdint>

class QCommandLineParser;

namespace qtllm::worker
{
struct WorkerOptions
{
    QString modelPath;
    QString prompt;
    QString systemPrompt = QStringLiteral("You are a helpful assistant.");
    QList<chat::Message> messages;
    inference::ResponseMode responseMode = inference::ResponseMode::Text;
    QString grammar;
    int contextSize = inference::defaultContextSize;
    int maxTokens = inference::defaultMaxOutputTokens;
    int threads = 0;
    inference::ModelLoadOptions modelLoadOptions;
    int topK = 40;
    float topP = 0.95F;
    float temperature = 0.6F;
    float repeatPenalty = 1.05F;
    std::uint32_t seed = 0xFFFFFFFFU;
};

void configureParser(QCommandLineParser& parser);
bool parseOptions(const QCommandLineParser& parser, WorkerOptions& options,
                  QString& errorMessage);
}  // namespace qtllm::worker
