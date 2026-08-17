#include "SettingsStore.hpp"

#include "ComputeProtocol.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
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

QString SettingsStore::workspacePath() const
{
    const QSettings settings(filePath_, QSettings::IniFormat);
    return settings.value(QStringLiteral("workspace/path"), QDir::homePath())
        .toString();
}

bool SettingsStore::setWorkspacePath(const QString& workspacePath) const
{
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("workspace/path"), workspacePath);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool SettingsStore::agentModeEnabled() const
{
    const QSettings settings(filePath_, QSettings::IniFormat);
    return settings.value(QStringLiteral("chat/agentMode"), false).toBool();
}

bool SettingsStore::setAgentModeEnabled(bool enabled) const
{
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("chat/agentMode"), enabled);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

inference::ModelLoadOptions SettingsStore::modelLoadOptions() const
{
    const QSettings settings(filePath_, QSettings::IniFormat);
    const auto serialized =
        settings.value(QStringLiteral("compute/modelLoadOptions"))
            .toString()
            .toUtf8();
    if (serialized.isEmpty()) return {};

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(serialized, &parseError);
    inference::ModelLoadOptions options;
    QString errorMessage;
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        !protocol::parseModelLoadOptions(document.object(), options,
                                         errorMessage))
        return {};
    return options;
}

bool SettingsStore::setModelLoadOptions(
    const inference::ModelLoadOptions& options) const
{
    QString errorMessage;
    if (!inference::validateModelLoadOptions(options, errorMessage))
        return false;
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(
        QStringLiteral("compute/modelLoadOptions"),
        QString::fromUtf8(
            QJsonDocument(protocol::serializeModelLoadOptions(options))
                .toJson(QJsonDocument::Compact)));
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

QList<mcp::McpServerConfig> SettingsStore::mcpServerConfigs() const
{
    QByteArray serialized;
    QFile configFile(mcpConfigFilePath());
    if (configFile.open(QIODevice::ReadOnly)) serialized = configFile.readAll();
    if (serialized.isEmpty())
    {
        const QSettings settings(filePath_, QSettings::IniFormat);
        serialized = settings.value(QStringLiteral("mcp/serversJson"))
                         .toString()
                         .toUtf8();
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(serialized, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray())
        return {};

    QList<mcp::McpServerConfig> configurations;
    for (const auto& value : document.array())
    {
        if (!value.isObject()) continue;
        mcp::McpServerConfig config;
        QString errorMessage;
        if (mcp::parseServerConfig(value.toObject(), config, errorMessage))
            configurations.append(std::move(config));
    }
    return configurations;
}

bool SettingsStore::setMcpServerConfigs(
    const QList<mcp::McpServerConfig>& configurations) const
{
    QJsonArray array;
    for (const auto& config : configurations)
        array.append(mcp::serializeServerConfig(config));
    QSaveFile configFile(mcpConfigFilePath());
    if (!configFile.open(QIODevice::WriteOnly)) return false;
    const auto serialized =
        QJsonDocument(array).toJson(QJsonDocument::Indented);
    if (configFile.write(serialized) != serialized.size() ||
        !configFile.commit())
        return false;
    return true;
}

bool SettingsStore::builtInFilesystemMcpEnabled() const
{
    const QSettings settings(filePath_, QSettings::IniFormat);
    return settings.value(QStringLiteral("mcp/builtInFilesystemEnabled"), true)
        .toBool();
}

bool SettingsStore::setBuiltInFilesystemMcpEnabled(bool enabled) const
{
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("mcp/builtInFilesystemEnabled"), enabled);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool SettingsStore::builtInFilesystemMcpUseInstructions() const
{
    const QSettings settings(filePath_, QSettings::IniFormat);
    return settings
        .value(QStringLiteral("mcp/builtInFilesystemUseInstructions"), true)
        .toBool();
}

bool SettingsStore::setBuiltInFilesystemMcpUseInstructions(bool enabled) const
{
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("mcp/builtInFilesystemUseInstructions"),
                      enabled);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QStringList SettingsStore::alwaysAllowedMcpTools() const
{
    const QSettings settings(filePath_, QSettings::IniFormat);
    auto tools =
        settings.value(QStringLiteral("mcp/alwaysAllowedTools")).toStringList();
    tools.removeAll({});
    tools.removeDuplicates();
    tools.sort(Qt::CaseSensitive);
    return tools;
}

bool SettingsStore::setAlwaysAllowedMcpTools(
    const QStringList& qualifiedToolNames) const
{
    auto tools = qualifiedToolNames;
    tools.removeAll({});
    tools.removeDuplicates();
    tools.sort(Qt::CaseSensitive);
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("mcp/alwaysAllowedTools"), tools);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QString SettingsStore::mcpConfigFilePath() const
{
    return QFileInfo(filePath_).absoluteDir().filePath(
        QStringLiteral("mcp-servers.json"));
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
