#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

namespace qtllm::models
{
enum class ModelOrigin
{
    ManagedPackage,
    ExternalPackage,
    DirectGguf
};

enum class VerificationStatus
{
    NotVerified,
    Verified,
    Invalid
};

struct ModelFileDescriptor
{
    QString path;
    qint64 sizeBytes = 0;
    QString sha256;
};

struct ModelUpstreamDescriptor
{
    QString publisher;
    QString modelId;
    QString revision;
    QString source;
};

struct ModelDescriptor
{
    int schemaVersion = 1;
    QString id;
    QString displayName;
    QString packageVersion;
    QString modelRevision;
    QString engine;
    QString format;
    QList<ModelFileDescriptor> modelFiles;
    QString minimumRuntimeVersion;
    int recommendedRamGb = 0;
    int defaultContextSize = 0;
    QString presetFile;
    QString licenseFile;
    ModelUpstreamDescriptor upstream;
    QString packageDirectory;
    ModelOrigin origin = ModelOrigin::DirectGguf;
    VerificationStatus verificationStatus = VerificationStatus::NotVerified;
};
}  // namespace qtllm::models
