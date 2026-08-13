#pragma once

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
    int contextSize = 4096;
    int maxTokens = 512;
    int threads = 0;
    int gpuLayers = 0;
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
