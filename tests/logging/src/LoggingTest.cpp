#include "Logging.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace qtllm::tests
{
class LoggingTest final : public QObject
{
    Q_OBJECT

   private slots:
    void writesApplicationAndQtMessages();
};

void LoggingTest::writesApplicationAndQtMessages()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto previousDirectory = qgetenv("QTLLM_LOG_DIR");
    QVERIFY(qputenv("QTLLM_LOG_DIR", directory.path().toUtf8()));

    QString errorMessage;
    QVERIFY2(logging::initialize(QStringLiteral("logging-test"), &errorMessage),
             qPrintable(errorMessage));
    const auto path = logging::logFilePath();
    QCOMPARE(logging::logDirectory(), QDir::cleanPath(directory.path()));
    QCOMPARE(
        path,
        QDir(directory.path()).filePath(QStringLiteral("logging-test.log")));

    logging::info(QStringLiteral("direct diagnostic message"));
    logging::installQtMessageHandler();
    qWarning().noquote() << "Qt diagnostic message";
    logging::shutdown();

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto contents = file.readAll();
    QVERIFY(contents.contains("direct diagnostic message"));
    QVERIFY(contents.contains("Qt diagnostic message"));

    if (previousDirectory.isNull())
        qunsetenv("QTLLM_LOG_DIR");
    else
        QVERIFY(qputenv("QTLLM_LOG_DIR", previousDirectory));
}
}  // namespace qtllm::tests

QTEST_GUILESS_MAIN(qtllm::tests::LoggingTest)

#include "LoggingTest.moc"
