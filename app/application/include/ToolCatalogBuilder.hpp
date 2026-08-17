#pragma once

#include "ToolDefinition.hpp"

#include <QByteArray>
#include <QList>

namespace qtllm::application
{
struct ToolCatalog
{
    QByteArray json = QByteArrayLiteral("[]");
    qsizetype includedToolCount = 0;
    qsizetype omittedToolCount = 0;
};

class ToolCatalogBuilder final
{
   public:
    static constexpr qsizetype defaultMaximumBytes = 65'536;

    [[nodiscard]] static ToolCatalog build(
        const QList<agent::ToolDefinition>& tools,
        qsizetype maximumBytes = defaultMaximumBytes);
};
}  // namespace qtllm::application
