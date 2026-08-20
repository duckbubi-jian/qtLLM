#pragma once

#include <QHash>
#include <QJsonArray>
#include <QString>
#include <QStringList>

namespace qtllm::application
{
class PlanTraceability final
{
   public:
    explicit PlanTraceability(const QString& originalRequest);

    [[nodiscard]] const QJsonArray& requestClauses() const;
    [[nodiscard]] QString validate(const QJsonArray& steps) const;
    [[nodiscard]] QJsonArray annotate(const QJsonArray& steps) const;

   private:
    [[nodiscard]] static QStringList splitRequest(const QString& request);
    [[nodiscard]] static QStringList literalAnchors(const QString& text);

    QJsonArray requestClauses_;
    QStringList clauseIds_;
    QHash<QString, QString> clauseTextById_;
};
}  // namespace qtllm::application
