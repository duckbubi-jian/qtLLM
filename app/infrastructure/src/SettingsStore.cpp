#include "SettingsStore.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

#include <utility>

namespace qtllm::infrastructure
{
namespace
{
QString defaultSettingsPath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("qtLLM.ini"));
}
}  // namespace

SettingsStore::SettingsStore(QString filePath)
    : filePath_(filePath.isEmpty() ? defaultSettingsPath()
                                   : std::move(filePath))
{
}

QString SettingsStore::filePath() const
{
    return filePath_;
}

QString SettingsStore::lastModelPath() const
{
    const QSettings settings(filePath_, QSettings::IniFormat);
    return settings.value(QStringLiteral("models/lastLoadedPath")).toString();
}

bool SettingsStore::setLastModelPath(const QString& modelPath) const
{
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("models/lastLoadedPath"), modelPath);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool SettingsStore::isModelFileVerified(const QString& modelPath,
                                        qint64 expectedSize,
                                        const QString& expectedSha256) const
{
    const QFileInfo modelInfo(modelPath);
    if (!modelInfo.exists() || !modelInfo.isFile() ||
        modelInfo.size() != expectedSize)
        return false;

    QSettings settings(filePath_, QSettings::IniFormat);
    settings.beginGroup(
        QStringLiteral("models/verified/%1")
            .arg(verificationKey(modelInfo.absoluteFilePath())));
    const auto matches =
        settings.value(QStringLiteral("path")).toString() ==
            modelInfo.absoluteFilePath() &&
        settings.value(QStringLiteral("sizeBytes")).toLongLong() ==
            expectedSize &&
        settings.value(QStringLiteral("lastModifiedMs")).toLongLong() ==
            modelInfo.lastModified().toMSecsSinceEpoch() &&
        settings.value(QStringLiteral("sha256")).toString() == expectedSha256;
    settings.endGroup();
    return matches;
}

bool SettingsStore::setModelFileVerified(const QString& modelPath,
                                         qint64 expectedSize,
                                         const QString& expectedSha256) const
{
    const QFileInfo modelInfo(modelPath);
    if (!modelInfo.exists() || !modelInfo.isFile() ||
        modelInfo.size() != expectedSize)
        return false;

    QSettings settings(filePath_, QSettings::IniFormat);
    settings.beginGroup(
        QStringLiteral("models/verified/%1")
            .arg(verificationKey(modelInfo.absoluteFilePath())));
    settings.setValue(QStringLiteral("path"), modelInfo.absoluteFilePath());
    settings.setValue(QStringLiteral("sizeBytes"), expectedSize);
    settings.setValue(QStringLiteral("lastModifiedMs"),
                      modelInfo.lastModified().toMSecsSinceEpoch());
    settings.setValue(QStringLiteral("sha256"), expectedSha256);
    settings.endGroup();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QString SettingsStore::verificationKey(const QString& modelPath)
{
    auto normalized = QDir::cleanPath(QDir::fromNativeSeparators(modelPath));
#ifdef Q_OS_WIN
    normalized = normalized.toLower();
#endif
    return QString::fromLatin1(
        QCryptographicHash::hash(normalized.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
}
}  // namespace qtllm::infrastructure
