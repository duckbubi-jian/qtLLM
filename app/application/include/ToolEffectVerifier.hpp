#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

namespace qtllm::application
{
struct ToolEffectExpectation
{
    QString field;
    QString normalizedField;
    QByteArray expectedDigest;
    QJsonValue expectedValue;
};

enum class ToolEffectVerificationStatus
{
    Verified,
    Incomplete,
    Mismatch
};

struct ToolEffectVerificationResult
{
    ToolEffectVerificationStatus status =
        ToolEffectVerificationStatus::Incomplete;
    QStringList matchedFields;
    QStringList missingFields;
    QStringList mismatchedFields;
};

class ToolEffectVerifier final
{
   public:
    [[nodiscard]] static QList<ToolEffectExpectation> captureExpectations(
        const QJsonObject& arguments);
    [[nodiscard]] static ToolEffectVerificationResult verify(
        const QList<ToolEffectExpectation>& expectations,
        const QJsonValue& readBack);
    [[nodiscard]] static bool explicitlyConfirmsVerification(
        const QJsonValue& value);
};
}  // namespace qtllm::application
