#pragma once

#include "WorkerOptions.hpp"

#include <QByteArray>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

namespace qtllm::worker
{
struct GenerationMetrics
{
    qint64 promptEvaluationMilliseconds = 0;
    qint64 firstTokenMilliseconds = -1;
    int promptTokens = 0;
    int generatedTokens = 0;
    double generationTokensPerSecond = 0.0;
};

class LlamaEngine
{
   public:
    using TokenHandler = std::function<void(const QByteArray&)>;

    LlamaEngine();
    ~LlamaEngine();

    LlamaEngine(const LlamaEngine&) = delete;
    LlamaEngine& operator=(const LlamaEngine&) = delete;

    bool loadModel(const QString& modelPath, int gpuLayers,
                   QString& errorMessage, qint64* loadMilliseconds = nullptr);
    void unloadModel();
    [[nodiscard]] bool isModelLoaded() const;
    [[nodiscard]] QString modelPath() const;

    bool generate(const WorkerOptions& options,
                  const TokenHandler& tokenHandler, QString& errorMessage,
                  GenerationMetrics* metrics = nullptr,
                  const std::atomic_bool* externalCancellation = nullptr);
    void cancel();

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic_bool cancelled_ = false;
};
}  // namespace qtllm::worker
