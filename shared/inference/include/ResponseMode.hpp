#pragma once

#include <QString>
#include <QStringView>

namespace qtllm::inference
{
enum class ResponseMode
{
    Text,
    AgentAction
};

QString responseModeName(ResponseMode mode);
bool parseResponseMode(QStringView name, ResponseMode& mode);
}  // namespace qtllm::inference
