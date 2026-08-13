#include "ChatMessage.hpp"

namespace qtllm::chat
{
QString roleName(Role role)
{
    switch (role)
    {
        case Role::System:
            return QStringLiteral("system");
        case Role::User:
            return QStringLiteral("user");
        case Role::Assistant:
            return QStringLiteral("assistant");
    }
    return {};
}

bool parseRole(QStringView name, Role& role)
{
    if (name == u"system")
        role = Role::System;
    else if (name == u"user")
        role = Role::User;
    else if (name == u"assistant")
        role = Role::Assistant;
    else
        return false;
    return true;
}
}  // namespace qtllm::chat
