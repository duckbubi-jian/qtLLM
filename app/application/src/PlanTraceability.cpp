#include "PlanTraceability.hpp"

#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace qtllm::application
{
namespace
{
void appendUnique(QStringList& values, const QString& candidate)
{
    const auto value = candidate.trimmed();
    if (value.isEmpty()) return;
    const auto exists = std::any_of(
        values.cbegin(), values.cend(), [&value](const QString& current)
        { return current.compare(value, Qt::CaseInsensitive) == 0; });
    if (!exists) values.append(value);
}
}  // namespace

PlanTraceability::PlanTraceability(const QString& originalRequest)
{
    const auto clauses = splitRequest(originalRequest);
    for (auto index = 0; index < clauses.size(); ++index)
    {
        const auto id = QStringLiteral("request-%1").arg(index + 1);
        const auto text = clauses.at(index);
        clauseIds_.append(id);
        clauseTextById_.insert(id, text);
        requestClauses_.append(QJsonObject{{QStringLiteral("id"), id},
                                           {QStringLiteral("text"), text}});
    }
}

const QJsonArray& PlanTraceability::requestClauses() const
{
    return requestClauses_;
}

QString PlanTraceability::validate(const QJsonArray& steps) const
{
    QSet<QString> coveredClauses;
    QHash<QString, QStringList> descriptionsByClause;
    for (const auto& value : steps)
    {
        const auto step = value.toObject();
        const auto stepId = step.value(QStringLiteral("id")).toString();
        const auto description =
            step.value(QStringLiteral("description")).toString();
        const auto sourceIds =
            step.value(QStringLiteral("source_ids")).toArray();
        for (const auto& sourceValue : sourceIds)
        {
            const auto sourceId = sourceValue.toString();
            if (!clauseTextById_.contains(sourceId))
                return QStringLiteral(
                           "Task-plan step '%1' refers to unknown source_id "
                           "'%2'. Use only IDs from request_clauses.")
                    .arg(stepId, sourceId);
            coveredClauses.insert(sourceId);
            descriptionsByClause[sourceId].append(description);
        }
    }

    QStringList uncovered;
    for (const auto& clauseId : clauseIds_)
        if (!coveredClauses.contains(clauseId)) uncovered.append(clauseId);
    if (!uncovered.isEmpty())
        return QStringLiteral(
                   "The task plan does not trace every original request "
                   "clause. Add these source_ids to the owning steps: %1.")
            .arg(uncovered.join(QStringLiteral(", ")));

    for (const auto& clauseId : clauseIds_)
    {
        const auto descriptions =
            descriptionsByClause.value(clauseId).join(QLatin1Char(' '));
        QStringList missing;
        for (const auto& literal :
             literalAnchors(clauseTextById_.value(clauseId)))
        {
            if (!descriptions.contains(literal, Qt::CaseInsensitive))
                missing.append(literal);
        }
        if (!missing.isEmpty())
            return QStringLiteral(
                       "The task plan dropped literal constraints from %1: "
                       "%2. Copy them exactly into the descriptions of the "
                       "steps mapped to that source_id.")
                .arg(clauseId, missing.join(QStringLiteral(", ")));
    }
    return {};
}

QJsonArray PlanTraceability::annotate(const QJsonArray& steps) const
{
    QJsonArray annotated;
    for (const auto& value : steps)
    {
        auto step = value.toObject();
        QJsonArray sourceRefs;
        for (const auto& sourceValue :
             step.value(QStringLiteral("source_ids")).toArray())
        {
            const auto sourceId = sourceValue.toString();
            sourceRefs.append(QJsonObject{
                {QStringLiteral("id"), sourceId},
                {QStringLiteral("text"), clauseTextById_.value(sourceId)}});
        }
        step.insert(QStringLiteral("source_refs"), sourceRefs);
        annotated.append(step);
    }
    return annotated;
}

QStringList PlanTraceability::splitRequest(const QString& request)
{
    static const QRegularExpression listMarker(
        QStringLiteral(R"(^\s*(?:\d{1,3}[.):]|[-*])\s*(.+?)\s*$)"));
    const auto lines =
        request.split(QRegularExpression(QStringLiteral("[\r\n]+")));
    const auto hasList =
        std::any_of(lines.cbegin(), lines.cend(), [](const QString& line)
                    { return listMarker.match(line).hasMatch(); });

    QStringList clauses;
    if (!hasList)
    {
        for (const auto& line : lines)
            if (!line.trimmed().isEmpty()) clauses.append(line.trimmed());
        if (clauses.isEmpty() && !request.trimmed().isEmpty())
            clauses.append(request.trimmed());
        return clauses;
    }

    QString preamble;
    QString current;
    bool sawListItem = false;
    for (const auto& line : lines)
    {
        const auto match = listMarker.match(line);
        if (match.hasMatch())
        {
            if (!sawListItem)
            {
                if (!preamble.isEmpty()) clauses.append(preamble);
                sawListItem = true;
            }
            else if (!current.isEmpty())
            {
                clauses.append(current);
            }
            current = match.captured(1).trimmed();
            continue;
        }
        const auto continuation = line.trimmed();
        if (continuation.isEmpty()) continue;
        if (!sawListItem)
        {
            if (!preamble.isEmpty()) preamble += QLatin1Char(' ');
            preamble += continuation;
        }
        else if (!current.isEmpty())
        {
            current += QLatin1Char(' ') + continuation;
        }
    }
    if (!current.isEmpty()) clauses.append(current);
    return clauses;
}

QStringList PlanTraceability::literalAnchors(const QString& text)
{
    QStringList anchors;
    static const QRegularExpression quoted(
        QStringLiteral(R"((['\"])([^'\"\r\n]{1,160})\1)"));
    auto match = quoted.globalMatch(text);
    while (match.hasNext())
        appendUnique(anchors, match.next().captured(2));

    static const QRegularExpression number(QStringLiteral(
        R"((?<![A-Za-z0-9_])[+-]?(?:\d+(?:\.\d+)?|\.\d+)(?:[eE][+-]?\d+)?(?![A-Za-z0-9_]))"));
    match = number.globalMatch(text);
    while (match.hasNext())
        appendUnique(anchors, match.next().captured(0));

    static const QRegularExpression pathOrFile(QStringLiteral(
        R"((?:[A-Za-z]:[\\/][^\s,;]+|[A-Za-z_][A-Za-z0-9_-]*\.[A-Za-z][A-Za-z0-9]{0,7}))"));
    match = pathOrFile.globalMatch(text);
    while (match.hasNext())
    {
        auto literal = match.next().captured(0);
        while (!literal.isEmpty() &&
               QStringLiteral(".\"')]}:").contains(literal.back()))
            literal.chop(1);
        appendUnique(anchors, literal);
    }
    return anchors;
}
}  // namespace qtllm::application
