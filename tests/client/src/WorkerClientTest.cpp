#include "WorkerClient.hpp"
#include "SettingsStore.hpp"

#include <QCoreApplication>
#include <QDir>
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
    void defaultsToApplicationDirectory();
    void startsHandshakesAndStopsWorker();
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
    QVERIFY(settings.setLastModelPath(modelPath));
    QCOMPARE(settings.lastModelPath(), modelPath);
    QVERIFY(QFileInfo::exists(settingsPath));

    const infrastructure::SettingsStore reloaded(settingsPath);
    QCOMPARE(reloaded.lastModelPath(), modelPath);
}

void WorkerClientTest::defaultsToApplicationDirectory()
{
    const infrastructure::SettingsStore settings;
    QCOMPARE(settings.filePath(), QDir(QCoreApplication::applicationDirPath())
                                      .filePath(QStringLiteral("qtLLM.ini")));
}

void WorkerClientTest::startsHandshakesAndStopsWorker()
{
    infrastructure::WorkerClient client;
    QSignalSpy errorSpy(&client, &infrastructure::WorkerClient::errorOccurred);

    client.start();
    QTRY_COMPARE_WITH_TIMEOUT(client.state(),
                              infrastructure::WorkerClient::State::Ready, 5000);
    QCOMPARE(errorSpy.count(), 0);

    client.stop();
    QTRY_COMPARE_WITH_TIMEOUT(
        client.state(), infrastructure::WorkerClient::State::Stopped, 5000);
    QCOMPARE(errorSpy.count(), 0);
}
}  // namespace qtllm::tests

QTEST_GUILESS_MAIN(qtllm::tests::WorkerClientTest)

#include "WorkerClientTest.moc"
