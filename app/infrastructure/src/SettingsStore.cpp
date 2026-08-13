#include "SettingsStore.hpp"

#include <QCoreApplication>
#include <QDir>
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
}  // namespace qtllm::infrastructure
