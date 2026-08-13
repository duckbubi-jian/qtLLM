#pragma once

#include "WorkerOptions.hpp"

#include <QByteArray>
#include <QString>

#include <atomic>
#include <functional>

namespace qtllm::worker
{
class LlamaEngine
{
   public:
    using TokenHandler = std::function<void(const QByteArray&)>;

    LlamaEngine();
    ~LlamaEngine();

    LlamaEngine(const LlamaEngine&) = delete;
    LlamaEngine& operator=(const LlamaEngine&) = delete;

    bool generate(const WorkerOptions& options,
                  const TokenHandler& tokenHandler, QString& errorMessage,
                  const std::atomic_bool* externalCancellation = nullptr);
    void cancel();

   private:
    std::atomic_bool cancelled_ = false;
};
}  // namespace qtllm::worker
