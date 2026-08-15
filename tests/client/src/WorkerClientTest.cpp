#include "WorkerClient.hpp"
#include "SettingsStore.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace qtllm::tests
{
class WorkerClientTest final : public QObject
{
    Q_OBJECT

   private slots:
    void storesLastModelPathInExplicitIniFile();
    void cachesVerifiedModelFingerprint();
    void defaultsToApplicationDirectory();
    void startsHandshakesAndStopsWorker();
    void reportsModelLoadFailure();
    void storesMcpServerConfiguration();
};

void WorkerClientTest::storesLastModelPathInExplicitIniFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto settingsPath =
        QDir(directory.path()).filePath(QStringLiteral("qtLLM.ini"));
    const auto modelPath = QStringLiteral("D:/models/deepseek/model.gguf");

    infrastructure::SettingsStore settings(settingsPath);
    QVERIFY(settings.lastModelPath().isEmpty());
    QCOMPARE(settings.workspacePath(), QDir::homePath());
    QVERIFY(!settings.agentModeEnabled());
    QVERIFY(settings.setLastModelPath(modelPath));
    QCOMPARE(settings.lastModelPath(), modelPath);
    const auto workspacePath = directory.path();
    QVERIFY(settings.setWorkspacePath(workspacePath));
    QCOMPARE(settings.workspacePath(), workspacePath);
    QVERIFY(settings.setAgentModeEnabled(true));
    QVERIFY(settings.agentModeEnabled());
    QVERIFY(QFileInfo::exists(settingsPath));

    const infrastructure::SettingsStore reloaded(settingsPath);
    QCOMPARE(reloaded.lastModelPath(), modelPath);
    QCOMPARE(reloaded.workspacePath(), workspacePath);
    QVERIFY(reloaded.agentModeEnabled());
}

void WorkerClientTest::defaultsToApplicationDirectory()
{
    const infrastructure::SettingsStore settings;
    QCOMPARE(settings.filePath(), QDir(QCoreApplication::applicationDirPath())
                                      .filePath(QStringLiteral("qtLLM.ini")));
}

void WorkerClientTest::cachesVerifiedModelFingerprint()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto settingsPath =
        QDir(directory.path()).filePath(QStringLiteral("qtLLM.ini"));
    const auto modelPath =
        QDir(directory.path()).filePath(QStringLiteral("model.gguf"));
    const auto contents = QByteArrayLiteral("GGUF-cache-test");
    QFile model(modelPath);
    QVERIFY(model.open(QIODevice::WriteOnly));
    QCOMPARE(model.write(contents), contents.size());
    model.close();
    const auto sha256 = QString::fromLatin1(
        QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());

    infrastructure::SettingsStore settings(settingsPath);
    QVERIFY(!settings.isModelFileVerified(modelPath, contents.size(), sha256));
    QVERIFY(settings.setModelFileVerified(modelPath, contents.size(), sha256));
    QVERIFY(settings.isModelFileVerified(modelPath, contents.size(), sha256));
    QVERIFY(!settings.isModelFileVerified(modelPath, contents.size(),
                                          QString(64, QLatin1Char('0'))));
    QVERIFY(
        !settings.isModelFileVerified(modelPath, contents.size() + 1, sha256));

    QVERIFY(model.open(QIODevice::ReadWrite));
    QVERIFY(model.seek(5));
    QCOMPARE(model.write("X", 1), qint64{1});
    QVERIFY(model.setFileTime(QDateTime::currentDateTimeUtc().addSecs(2),
                              QFileDevice::FileModificationTime));
    model.close();
    QVERIFY(!settings.isModelFileVerified(modelPath, contents.size(), sha256));
}

void WorkerClientTest::startsHandshakesAndStopsWorker()
{
    infrastructure::WorkerClient client;
    QSignalSpy errorSpy(&client, &infrastructure::WorkerClient::errorOccurred);

    client.start();
    QTRY_COMPARE_WITH_TIMEOUT(client.state(),
                              infrastructure::WorkerClient::State::Ready, 5000);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(client.capabilities().structuredGeneration, true);
    QCOMPARE(client.capabilities().grammar, true);

    client.stop();
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), infrastructure::WorkerClient::State::Stopped, 5000);
    QCOMPARE(errorSpy.count(), 0);
}

void WorkerClientTest::reportsModelLoadFailure()
{
    infrastructure::WorkerClient client;
    QSignalSpy loadFailureSpy(&client,
                              &infrastructure::WorkerClient::modelLoadFailed);

    client.start();
    QTRY_COMPARE_WITH_TIMEOUT(client.state(),
                              infrastructure::WorkerClient::State::Ready, 5000);
    client.loadModel(QCoreApplication::applicationFilePath());
    QTRY_COMPARE_WITH_TIMEOUT(loadFailureSpy.count(), 1, 5000);
    QCOMPARE(client.state(), infrastructure::WorkerClient::State::Ready);
    QVERIFY(!loadFailureSpy.constFirst().at(0).toString().isEmpty());
    QVERIFY(!loadFailureSpy.constFirst().at(1).toString().isEmpty());
}

void WorkerClientTest::storesMcpServerConfiguration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto settingsPath =
        QDir(directory.path()).filePath(QStringLiteral("qtLLM.ini"));
    infrastructure::mcp::McpServerConfig config;
    config.serverId = QStringLiteral("fake");
    config.program = QCoreApplication::applicationFilePath();
    config.arguments = {QStringLiteral("--stdio")};
    config.environment.insert(QStringLiteral("TOKEN"), QStringLiteral("value"));
    config.toolAllowlist = {QStringLiteral("echo")};

    infrastructure::SettingsStore settings(settingsPath);
    QVERIFY(settings.setMcpServerConfigs({config}));
    const auto loaded = settings.mcpServerConfigs();
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.constFirst().serverId, config.serverId);
    QCOMPARE(loaded.constFirst().program, config.program);
    QCOMPARE(loaded.constFirst().arguments, config.arguments);
    QCOMPARE(loaded.constFirst().environment, config.environment);
    QCOMPARE(loaded.constFirst().toolAllowlist, config.toolAllowlist);

    const QStringList alwaysAllowed{
        QStringLiteral("filesystem.write_file"),
        QStringLiteral("filesystem.create_directory")};
    QVERIFY(settings.setAlwaysAllowedMcpTools(alwaysAllowed));
    QCOMPARE(settings.alwaysAllowedMcpTools(),
             QStringList({QStringLiteral("filesystem.create_directory"),
                          QStringLiteral("filesystem.write_file")}));
}
}  // namespace qtllm::tests

QTEST_GUILESS_MAIN(qtllm::tests::WorkerClientTest)

#include "WorkerClientTest.moc"
