#pragma once

#include "InferenceDefaults.hpp"
#include "ModelDescriptor.hpp"

#include <QString>
#include <QtGlobal>

#include <functional>

namespace qtllm::models
{
struct InferencePreset
{
    int contextSize = inference::defaultContextSize;
    int maxOutputTokens = inference::defaultMaxOutputTokens;
    float temperature = 0.6F;
    float topP = 0.95F;
    int topK = 40;
    float repeatPenalty = 1.05F;
};

struct ModelSelection
{
    ModelDescriptor descriptor;
    InferencePreset preset;
    QString selectedPath;
    QString modelPath;
};

struct ModelPackageResult
{
    ModelSelection selection;
    QString errorCode;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const
    {
        return errorCode.isEmpty();
    }
};

class ModelPackage final
{
   public:
    using VerificationProgress = std::function<void(quint64, quint64)>;

    [[nodiscard]] static ModelPackageResult inspect(
        const QString& selectedPath, const QString& runtimeVersion);
    [[nodiscard]] static ModelPackageResult verify(
        ModelSelection selection, VerificationProgress progress = {});
};
}  // namespace qtllm::models
