#include "AgentTask.hpp"

#include <utility>

namespace qtllm::application
{
AgentTask::AgentTask(Kind kind, QString id, QString description)
    : kind_(kind), id_(std::move(id)), description_(std::move(description))
{
}

AgentTask::~AgentTask() = default;

QString AgentTask::id() const
{
    return id_;
}

QString AgentTask::description() const
{
    return description_;
}

AgentTask::Kind AgentTask::kind() const
{
    return kind_;
}

QString AgentTask::kindName() const
{
    return kindName(kind_);
}

QString AgentTask::kindName(Kind kind)
{
    switch (kind)
    {
        case Kind::Planning:
            return QStringLiteral("planning");
        case Kind::Execution:
            return QStringLiteral("execution");
        case Kind::Summary:
            return QStringLiteral("summary");
    }
    return QStringLiteral("unknown");
}

AgentTask::Status AgentTask::status() const
{
    return status_;
}

AgentTask::Snapshot AgentTask::runtimeSnapshot() const
{
    return {id_,        description_, activity(),
            kind_,      status_,      createdAt_,
            startedAt_, finishedAt_,  elapsedMilliseconds()};
}

bool AgentTask::hasConversation() const
{
    return !messages_.isEmpty();
}

QList<chat::Message>& AgentTask::messages()
{
    return messages_;
}

const QList<chat::Message>& AgentTask::messages() const
{
    return messages_;
}

qsizetype& AgentTask::requestMessageIndex()
{
    return requestMessageIndex_;
}

bool AgentTask::appendCorrectionTurn(const QByteArray& rawAction,
                                     chat::Message correction,
                                     bool recordAction)
{
    if (messages_.isEmpty() || correction.role != chat::Role::User ||
        correction.content.isEmpty())
        return false;

    const auto expectedTailRole =
        recordAction ? chat::Role::User : chat::Role::Assistant;
    if (messages_.constLast().role != expectedTailRole) return false;
    if (recordAction)
    {
        if (rawAction.isEmpty()) return false;
        messages_.append({chat::Role::Assistant, QString::fromUtf8(rawAction)});
    }
    messages_.append(std::move(correction));
    return true;
}

void AgentTask::requestDecision(const GenerateHandler& generate,
                                const models::InferencePreset& preset,
                                int outputTokens)
{
    decisionBytes_.clear();
    setStatus(Status::WaitingForModel);
    generate(messages_, preset, outputTokens);
}

bool AgentTask::receiveToken(const QByteArray& bytes, qsizetype maximumBytes)
{
    if (bytes.isEmpty()) return true;
    decisionBytes_ += bytes;
    return decisionBytes_.size() <= maximumBytes;
}

AgentTask::Decision AgentTask::completeDecision(bool cancelled)
{
    setStatus(Status::Running);
    Decision result;
    result.rawAction = decisionBytes_;
    if (cancelled)
    {
        result.errorMessage =
            QStringLiteral("Agent decision generation was cancelled.");
        return result;
    }
    result.valid = agent::parseAction(result.rawAction, result.action,
                                      result.errorMessage);
    return result;
}

void AgentTask::cancel()
{
    setStatus(Status::Cancelled);
}

void AgentTask::fail()
{
    setStatus(Status::Failed);
}

void AgentTask::activateConversation(QList<chat::Message> messages,
                                     qsizetype requestMessageIndex)
{
    messages_ = std::move(messages);
    requestMessageIndex_ = requestMessageIndex;
    decisionBytes_.clear();
    if (!startedTick_.has_value())
    {
        startedAt_ = QDateTime::currentDateTimeUtc();
        startedTick_ = std::chrono::steady_clock::now();
    }
    setStatus(Status::Running);
}

void AgentTask::setStatus(Status status)
{
    if (isTerminal(status_)) return;
    const auto elapsedBeforeTransition = elapsedMilliseconds();
    status_ = status;
    if (!isTerminal(status)) return;
    terminalElapsedMilliseconds_ = elapsedBeforeTransition;
    finishedAt_ = QDateTime::currentDateTimeUtc();
}

void AgentTask::complete()
{
    setStatus(Status::Completed);
}

void AgentTask::block()
{
    setStatus(Status::Blocked);
}

bool AgentTask::isTerminal(Status status)
{
    return status == Status::Completed || status == Status::Blocked ||
           status == Status::Cancelled || status == Status::Failed;
}

qint64 AgentTask::elapsedMilliseconds() const
{
    if (!startedAt_.isValid()) return 0;
    if (isTerminal(status_)) return terminalElapsedMilliseconds_;
    if (!startedTick_.has_value()) return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - *startedTick_)
        .count();
}
}  // namespace qtllm::application
