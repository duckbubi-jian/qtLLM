#include "WorkerOptions.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QThread>

#include <cmath>
#include <limits>

namespace qtllm::worker
{
namespace
{
template <typename Value>
bool parseInteger(const QCommandLineParser& parser, const QString& optionName,
                  Value minimum, Value maximum, Value& destination,
                  QString& errorMessage)
{
    bool ok = false;
    const auto parsed = parser.value(optionName).toLongLong(&ok);
    if (!ok || parsed < static_cast<qint64>(minimum) ||
        parsed > static_cast<qint64>(maximum))
    {
        errorMessage = QStringLiteral("Invalid --%1 value: %2")
                           .arg(optionName, parser.value(optionName));
        return false;
    }

    destination = static_cast<Value>(parsed);
    return true;
}

bool parseFloat(const QCommandLineParser& parser, const QString& optionName,
                float minimum, float maximum, float& destination,
                QString& errorMessage)
{
    bool ok = false;
    const auto parsed = parser.value(optionName).toFloat(&ok);
    if (!ok || !std::isfinite(parsed) || parsed < minimum || parsed > maximum)
    {
        errorMessage = QStringLiteral("Invalid --%1 value: %2")
                           .arg(optionName, parser.value(optionName));
        return false;
    }

    destination = parsed;
    return true;
}
}  // namespace

void configureParser(QCommandLineParser& parser)
{
    parser.setApplicationDescription(QStringLiteral(
        "Load a local GGUF model and stream one chat response."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(
        {QStringLiteral("model"),
         QStringLiteral("Path to a GGUF model or its first split."),
         QStringLiteral("path")});
    parser.addOption({QStringLiteral("prompt"),
                      QStringLiteral("User prompt. Reads stdin when omitted."),
                      QStringLiteral("text")});
    parser.addOption({QStringLiteral("system-prompt"),
                      QStringLiteral("System prompt."), QStringLiteral("text"),
                      QStringLiteral("You are a helpful assistant.")});
    parser.addOption({QStringLiteral("context-size"),
                      QStringLiteral("Context size in tokens."),
                      QStringLiteral("tokens"), QStringLiteral("4096")});
    parser.addOption({QStringLiteral("max-tokens"),
                      QStringLiteral("Maximum generated tokens."),
                      QStringLiteral("tokens"), QStringLiteral("512")});
    parser.addOption(
        {QStringLiteral("threads"),
         QStringLiteral("CPU threads; 0 selects the Qt recommendation."),
         QStringLiteral("count"), QStringLiteral("0")});
    parser.addOption(
        {QStringLiteral("gpu-layers"),
         QStringLiteral("Layers to offload; CPU build defaults to 0."),
         QStringLiteral("count"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("temperature"),
                      QStringLiteral("Sampling temperature."),
                      QStringLiteral("value"), QStringLiteral("0.6")});
    parser.addOption({QStringLiteral("top-p"),
                      QStringLiteral("Nucleus sampling probability."),
                      QStringLiteral("value"), QStringLiteral("0.95")});
    parser.addOption({QStringLiteral("top-k"),
                      QStringLiteral("Top-k sampling candidates."),
                      QStringLiteral("count"), QStringLiteral("40")});
    parser.addOption({QStringLiteral("repeat-penalty"),
                      QStringLiteral("Repeat penalty; 1 disables it."),
                      QStringLiteral("value"), QStringLiteral("1.05")});
    parser.addOption(
        {QStringLiteral("seed"),
         QStringLiteral("Random seed; 4294967295 selects a random seed."),
         QStringLiteral("value"),
         QString::number(std::numeric_limits<std::uint32_t>::max())});
}

bool parseOptions(const QCommandLineParser& parser, WorkerOptions& options,
                  QString& errorMessage)
{
    options.modelPath = parser.value(QStringLiteral("model"));
    if (options.modelPath.isEmpty())
    {
        errorMessage = QStringLiteral("Missing required --model option.");
        return false;
    }

    const QFileInfo modelInfo(options.modelPath);
    if (!modelInfo.exists() || !modelInfo.isFile())
    {
        errorMessage = QStringLiteral("Model file does not exist: %1")
                           .arg(options.modelPath);
        return false;
    }
    options.modelPath = modelInfo.absoluteFilePath();

    options.prompt = parser.value(QStringLiteral("prompt"));
    options.systemPrompt = parser.value(QStringLiteral("system-prompt"));

    if (!parseInteger(parser, QStringLiteral("context-size"), 256, 1'048'576,
                      options.contextSize, errorMessage) ||
        !parseInteger(parser, QStringLiteral("max-tokens"), 1, 1'048'576,
                      options.maxTokens, errorMessage) ||
        !parseInteger(parser, QStringLiteral("threads"), 0, 1024,
                      options.threads, errorMessage) ||
        !parseInteger(parser, QStringLiteral("gpu-layers"), 0, 10'000,
                      options.gpuLayers, errorMessage) ||
        !parseInteger(parser, QStringLiteral("top-k"), 0, 1'000'000,
                      options.topK, errorMessage) ||
        !parseInteger(parser, QStringLiteral("seed"), std::uint32_t{0},
                      std::numeric_limits<std::uint32_t>::max(), options.seed,
                      errorMessage) ||
        !parseFloat(parser, QStringLiteral("temperature"), 0.0F, 10.0F,
                    options.temperature, errorMessage) ||
        !parseFloat(parser, QStringLiteral("top-p"), 0.0F, 1.0F, options.topP,
                    errorMessage) ||
        !parseFloat(parser, QStringLiteral("repeat-penalty"), 0.0F, 10.0F,
                    options.repeatPenalty, errorMessage))
    {
        return false;
    }

    if (options.threads == 0)
    {
        options.threads = qMax(1, QThread::idealThreadCount());
    }

    return true;
}
}  // namespace qtllm::worker
