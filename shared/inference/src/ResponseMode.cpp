#include "ResponseMode.hpp"

namespace qtllm::inference
{
QString responseModeName(ResponseMode mode)
{
    switch (mode)
    {
        case ResponseMode::Text:
            return QStringLiteral("text");
        case ResponseMode::AgentAction:
            return QStringLiteral("agent_action");
    }
    return {};
}

bool parseResponseMode(QStringView name, ResponseMode& mode)
{
    if (name == u"text")
        mode = ResponseMode::Text;
    else if (name == u"agent_action")
        mode = ResponseMode::AgentAction;
    else
        return false;
    return true;
}
}  // namespace qtllm::inference
