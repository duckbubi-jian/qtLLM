#pragma once

#include <QString>

namespace qtllm::agent
{
struct ToolValidationIssue
{
    QString toolName;
    QString instancePath;
    QString schemaPath;
    QString keyword;
    QString message;
};

struct ToolValidationResult
{
    bool valid = false;
    ToolValidationIssue issue;
};
}  // namespace qtllm::agent
