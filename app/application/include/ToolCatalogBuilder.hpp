#pragma once

#include "ToolDefinition.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>

namespace qtllm::application
{
struct ToolCatalog
{
    QByteArray json = QByteArrayLiteral("[]");
    QByteArray indexJson = QByteArrayLiteral("[]");
    qsizetype includedToolCount = 0;
    qsizetype omittedToolCount = 0;
    qsizetype indexedToolCount = 0;
    qsizetype unindexedToolCount = 0;
    QStringList omittedToolNames;
    QStringList unindexedToolNames;
};

class ToolCatalogBuilder final
{
   public:
    static constexpr qsizetype defaultMaximumBytes = 65'536;
    static constexpr qsizetype defaultMaximumIndexBytes = 16'384;

    [[nodiscard]] static ToolCatalog build(
        const QList<agent::ToolDefinition>& tools,
        qsizetype maximumBytes = defaultMaximumBytes,
        qsizetype maximumIndexBytes = defaultMaximumIndexBytes);
    [[nodiscard]] static QJsonObject compactDefinition(
        const agent::ToolDefinition& tool);
};
}  // namespace qtllm::application
