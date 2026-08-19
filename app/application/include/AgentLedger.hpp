#pragma once

#include "AgentAction.hpp"
#include "ToolResult.hpp"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace qtllm::application
{
enum class ToolOperationKind
{
    Unknown,
    ReadOnly,
    Mutation
};

struct AgentResourceRecord
{
    QString serverId;
    QString kind;
    QString name;
    QString stableId;
    QString stableIdField;
    QString parentId;
    QString parentIdField;
    QString sourceTool;
    QString sourcePath;
    int sourceEvidenceSequence = 0;
};

struct AgentJobRecord
{
    QString serverId;
    QString tool;
    QString jobId;
    QString status;
    bool terminal = false;
    qint64 lastPollAtMs = 0;
    QJsonObject pollArguments;
    int sourceEvidenceSequence = 0;
    QString pollKey;
};

struct AgentVerificationRecord
{
    int mutationEvidenceSequence = 0;
    QString tool;
    QString state;
    QStringList targetIds;
    QStringList targetNames;
    int verificationEvidenceSequence = 0;
};

class AgentLedger final
{
   public:
    void clear();
    void recordToolResult(int evidenceSequence, const agent::Action& action,
                          const agent::ToolResult& result,
                          ToolOperationKind operationKind);

    [[nodiscard]] const QList<AgentResourceRecord>& resources() const;
    [[nodiscard]] const QList<AgentJobRecord>& jobs() const;
    [[nodiscard]] const QList<AgentVerificationRecord>& verifications() const;
    [[nodiscard]] bool hasUnresolvedVerification() const;
    [[nodiscard]] bool evidenceRequiresVerification(int sequence) const;
    [[nodiscard]] QString unresolvedVerificationReason() const;
    [[nodiscard]] QJsonObject snapshot() const;

   private:
    QList<AgentResourceRecord> resources_;
    QList<AgentJobRecord> jobs_;
    QList<AgentVerificationRecord> verifications_;
};
}  // namespace qtllm::application
