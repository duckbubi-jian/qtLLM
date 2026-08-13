#pragma once

#include <QString>
#include <QStringView>

namespace qtllm::chat
{
enum class Role
{
    System,
    User,
    Assistant
};

struct Message
{
    Role role = Role::User;
    QString content;

    bool operator==(const Message&) const = default;
};

QString roleName(Role role);
bool parseRole(QStringView name, Role& role);
}  // namespace qtllm::chat
