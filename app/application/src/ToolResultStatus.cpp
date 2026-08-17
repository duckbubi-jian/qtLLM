#include "ToolResultStatus.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace qtllm::application
{
namespace
{
enum class ProgressSignal
{
    None,
    InProgress,
    Terminal
};

constexpr auto maximumTraversalDepth = 32;

ProgressSignal mergeSignal(ProgressSignal left, ProgressSignal right)
{
    if (left == ProgressSignal::Terminal || right == ProgressSignal::Terminal)
        return ProgressSignal::Terminal;
    if (left == ProgressSignal::InProgress ||
        right == ProgressSignal::InProgress)
        return ProgressSignal::InProgress;
    return ProgressSignal::None;
}

ProgressSignal stateStringSignal(const QString& value)
{
    const auto state = value.trimmed().toLower();
    static const QSet<QString> inProgressStates{
        QStringLiteral("running"),     QStringLiteral("pending"),
        QStringLiteral("queued"),      QStringLiteral("in_progress"),
        QStringLiteral("in-progress"), QStringLiteral("in progress"),
        QStringLiteral("processing")};
    if (inProgressStates.contains(state)) return ProgressSignal::InProgress;

    static const QSet<QString> terminalStates{
        QStringLiteral("complete"),  QStringLiteral("completed"),
        QStringLiteral("success"),   QStringLiteral("succeeded"),
        QStringLiteral("failed"),    QStringLiteral("error"),
        QStringLiteral("cancelled"), QStringLiteral("canceled"),
        QStringLiteral("stopped"),   QStringLiteral("finished"),
        QStringLiteral("done"),      QStringLiteral("idle")};
    return terminalStates.contains(state) ? ProgressSignal::Terminal
                                          : ProgressSignal::None;
}

ProgressSignal progressSignal(const QJsonValue& value, int depth)
{
    if (depth > maximumTraversalDepth) return ProgressSignal::None;
    if (value.isArray())
    {
        auto signal = ProgressSignal::None;
        for (const auto& item : value.toArray())
            signal = mergeSignal(signal, progressSignal(item, depth + 1));
        return signal;
    }
    if (!value.isObject()) return ProgressSignal::None;

    const auto object = value.toObject();
    auto signal = ProgressSignal::None;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
    {
        const auto key = it.key().toLower();
        if (key == QStringLiteral("isrunning") && it.value().isBool())
            signal = mergeSignal(signal, it.value().toBool()
                                             ? ProgressSignal::InProgress
                                             : ProgressSignal::Terminal);
        else if ((key == QStringLiteral("status") ||
                  key == QStringLiteral("state")) &&
                 it.value().isString())
            signal =
                mergeSignal(signal, stateStringSignal(it.value().toString()));
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        signal = mergeSignal(signal, progressSignal(it.value(), depth + 1));
    return signal;
}
}  // namespace

bool toolResultIndicatesInProgress(const agent::ToolResult& result)
{
    if (result.isError) return false;
    auto payload = result.structuredContent;
    if (payload.isUndefined() || payload.isNull())
    {
        payload = result.result.value(QStringLiteral("structuredContent"));
        if (payload.isUndefined() || payload.isNull()) payload = result.result;
    }
    return progressSignal(payload, 0) == ProgressSignal::InProgress;
}
}  // namespace qtllm::application
