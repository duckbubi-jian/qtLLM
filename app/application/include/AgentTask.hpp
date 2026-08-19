#pragma once

#include "AgentAction.hpp"
#include "ChatMessage.hpp"
#include "ModelPackage.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QString>

#include <functional>

namespace qtllm::application
{
class AgentTask
{
   public:
    enum class Kind
    {
        Planning,
        Execution,
        Summary
    };

    enum class Status
    {
        Pending,
        Running,
        WaitingForModel,
        WaitingForApproval,
        WaitingForTool,
        Completed,
        Blocked,
        Cancelled,
        Failed
    };

    using GenerateHandler = std::function<void(
        const QList<chat::Message>&, const models::InferencePreset&, int)>;

    struct Decision
    {
        bool valid = false;
        agent::Action action;
        QByteArray rawAction;
        QString errorMessage;
    };

    struct Snapshot
    {
        QString id;
        QString description;
        QString activity;
        Kind kind = Kind::Execution;
        Status status = Status::Pending;
        QDateTime createdAt;
        QDateTime startedAt;
        QDateTime finishedAt;
        qint64 elapsedMilliseconds = 0;
    };

    AgentTask(Kind kind, QString id, QString description);
    virtual ~AgentTask();

    [[nodiscard]] QString id() const;
    [[nodiscard]] QString description() const;
    [[nodiscard]] Kind kind() const;
    [[nodiscard]] Status status() const;
    [[nodiscard]] Snapshot runtimeSnapshot() const;

    [[nodiscard]] bool hasConversation() const;
    [[nodiscard]] QList<chat::Message>& messages();
    [[nodiscard]] const QList<chat::Message>& messages() const;
    [[nodiscard]] qsizetype& requestMessageIndex();

    void requestDecision(const GenerateHandler& generate,
                         const models::InferencePreset& preset,
                         int outputTokens);
    [[nodiscard]] bool receiveToken(const QByteArray& bytes,
                                    qsizetype maximumBytes);
    [[nodiscard]] Decision completeDecision(bool cancelled);

    void cancel();
    void fail();

   protected:
    void activateConversation(QList<chat::Message> messages,
                              qsizetype requestMessageIndex);
    void setStatus(Status status);
    void complete();
    void block();
    [[nodiscard]] virtual QString activity() const = 0;

   private:
    [[nodiscard]] static bool isTerminal(Status status);
    [[nodiscard]] qint64 elapsedMilliseconds() const;

    Kind kind_;
    QString id_;
    QString description_;
    Status status_ = Status::Pending;
    QList<chat::Message> messages_;
    qsizetype requestMessageIndex_ = 0;
    QByteArray decisionBytes_;
    QDateTime createdAt_ = QDateTime::currentDateTimeUtc();
    QDateTime startedAt_;
    QDateTime finishedAt_;
    QElapsedTimer elapsedTimer_;
    qint64 terminalElapsedMilliseconds_ = 0;
};
}  // namespace qtllm::application
