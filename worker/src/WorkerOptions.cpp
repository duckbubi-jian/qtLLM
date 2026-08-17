#include "WorkerOptions.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QStringList>
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

bool parseModelPlacement(const QCommandLineParser& parser,
                         inference::ModelLoadOptions& options,
                         QString& errorMessage)
{
    if (!inference::parseDevicePlacementMode(
            parser.value(QStringLiteral("gpu-mode")), options.placementMode))
    {
        errorMessage = QStringLiteral("Invalid --gpu-mode value: %1")
                           .arg(parser.value(QStringLiteral("gpu-mode")));
        return false;
    }

    auto deviceIds = parser.value(QStringLiteral("gpu-devices"))
                         .split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (auto& id : deviceIds)
        id = id.trimmed();
    const auto weightValues = parser.value(QStringLiteral("tensor-split"))
                                  .split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (!weightValues.isEmpty() && weightValues.size() != deviceIds.size())
    {
        errorMessage = QStringLiteral(
            "--tensor-split must contain one weight per --gpu-devices entry.");
        return false;
    }
    for (qsizetype index = 0; index < deviceIds.size(); ++index)
    {
        auto weight = 1.0F;
        if (!weightValues.isEmpty())
        {
            bool ok = false;
            weight = weightValues.at(index).trimmed().toFloat(&ok);
            if (!ok || !std::isfinite(weight) || weight <= 0.0F)
            {
                errorMessage =
                    QStringLiteral("Invalid --tensor-split value: %1")
                        .arg(weightValues.at(index));
                return false;
            }
        }
        options.devices.append({deviceIds.at(index), weight});
    }
    return inference::validateModelLoadOptions(options, errorMessage);
}
}  // namespace

void configureParser(QCommandLineParser& parser)
{
    parser.setApplicationDescription(QStringLiteral(
        "Load a local GGUF model and stream one chat response."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(
        {QStringLiteral("ipc"),
         QStringLiteral("Run as a persistent JSON Lines worker.")});
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
                      QStringLiteral("tokens"),
                      QString::number(inference::defaultContextSize)});
    parser.addOption({QStringLiteral("max-tokens"),
                      QStringLiteral("Maximum generated tokens."),
                      QStringLiteral("tokens"),
                      QString::number(inference::defaultMaxOutputTokens)});
    parser.addOption(
        {QStringLiteral("threads"),
         QStringLiteral("CPU threads; 0 selects the Qt recommendation."),
         QStringLiteral("count"), QStringLiteral("0")});
    parser.addOption(
        {QStringLiteral("gpu-layers"),
         QStringLiteral(
             "Layers to offload; -1 uses all layers when a GPU is available."),
         QStringLiteral("count"), QStringLiteral("-1")});
    parser.addOption(
        {QStringLiteral("gpu-mode"),
         QStringLiteral("GPU placement: auto, cpu, single, or custom."),
         QStringLiteral("mode"), QStringLiteral("auto")});
    parser.addOption(
        {QStringLiteral("gpu-devices"),
         QStringLiteral("Comma-separated GPU IDs for single or custom mode."),
         QStringLiteral("ids")});
    parser.addOption(
        {QStringLiteral("tensor-split"),
         QStringLiteral("Comma-separated positive custom GPU weights."),
         QStringLiteral("weights")});
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
        !parseInteger(parser, QStringLiteral("gpu-layers"), -1, 10'000,
                      options.modelLoadOptions.gpuLayers, errorMessage) ||
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

    if (!parseModelPlacement(parser, options.modelLoadOptions, errorMessage))
        return false;

    if (options.threads == 0)
    {
        options.threads = qMax(1, QThread::idealThreadCount());
    }

    return true;
}
}  // namespace qtllm::worker
