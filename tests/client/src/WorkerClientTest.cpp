#include "WorkerClient.hpp"

#include <QSignalSpy>
#include <QtTest>

namespace qtllm::tests
{
class WorkerClientTest final : public QObject
{
    Q_OBJECT

   private slots:
    void startsHandshakesAndStopsWorker();
};

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
