#include "LlamaEngine.hpp"
#include "StdinReader.hpp"
#include "WorkerOptions.hpp"
#include "WorkerSession.hpp"

#include "Logging.hpp"
#include "ProtocolVersion.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QThread>

#include <atomic>
#include <csignal>

namespace
{
std::atomic_bool interrupted = false;

class LoggingGuard final
{
   public:
    ~LoggingGuard()
    {
        qtllm::logging::shutdown();
    }
};

void handleInterrupt(int)
{
    interrupted.store(true, std::memory_order_relaxed);
}

int runIpc(QCoreApplication& application)
{
    qtllm::worker::WorkerSession session;
    QString errorMessage;
    if (!session.start(errorMessage))
    {
        QTextStream(stderr) << "error: " << errorMessage << Qt::endl;
        return 2;
    }

    QThread readerThread;
    qtllm::worker::StdinReader reader;
    reader.moveToThread(&readerThread);
    QObject::connect(&readerThread, &QThread::started, &reader,
                     &qtllm::worker::StdinReader::readLines);
    QObject::connect(&reader, &qtllm::worker::StdinReader::lineReceived,
                     &session, &qtllm::worker::WorkerSession::processLine);
    QObject::connect(&reader, &qtllm::worker::StdinReader::inputClosed,
                     &application, &QCoreApplication::quit);
    QObject::connect(&reader, &qtllm::worker::StdinReader::inputClosed,
                     &readerThread, &QThread::quit);
    readerThread.start();

    const auto result = application.exec();
    readerThread.quit();
    readerThread.wait();
    return result;
}
}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qtllm-worker"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral("0.2.0 (protocol %1)").arg(qtllm::protocol::version));

    QString loggingError;
    if (!qtllm::logging::initialize(QStringLiteral("worker"), &loggingError))
        QTextStream(stderr) << "warning: " << loggingError << Qt::endl;
    qtllm::logging::installQtMessageHandler();
    const LoggingGuard loggingGuard;
    qtllm::logging::info(
        QStringLiteral("qtllm-worker 0.2.0 starting; protocol=%1 log=%2")
            .arg(qtllm::protocol::version)
            .arg(qtllm::logging::logFilePath()));

    QCommandLineParser parser;
    qtllm::worker::configureParser(parser);
    parser.process(application);

    if (parser.isSet(QStringLiteral("ipc")))
    {
        return runIpc(application);
    }

    qtllm::worker::WorkerOptions options;
    QString errorMessage;
    if (!qtllm::worker::parseOptions(parser, options, errorMessage))
    {
        QTextStream(stderr) << "error: " << errorMessage << Qt::endl;
        return 2;
    }

    if (options.prompt.isEmpty())
    {
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly))
        {
            QTextStream(stderr)
                << "error: unable to read prompt from stdin" << Qt::endl;
            return 2;
        }
        options.prompt = QString::fromUtf8(input.readAll()).trimmed();
    }
    if (options.prompt.isEmpty())
    {
        QTextStream(stderr) << "error: prompt is empty" << Qt::endl;
        return 2;
    }

    std::signal(SIGINT, handleInterrupt);

    qtllm::worker::LlamaEngine engine;
    qint64 loadMilliseconds = 0;
    if (!engine.loadModel(options.modelPath, options.modelLoadOptions,
                          errorMessage, &loadMilliseconds))
    {
        QTextStream(stderr) << "error: " << errorMessage << Qt::endl;
        return 1;
    }
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly))
    {
        QTextStream(stderr) << "error: unable to open stdout" << Qt::endl;
        return 2;
    }
    const auto generated = engine.generate(
        options,
        [&output, &engine](const QByteArray& piece)
        {
            if (interrupted.load(std::memory_order_relaxed))
            {
                engine.cancel();
                return;
            }
            output.write(piece);
            output.flush();
        },
        errorMessage, nullptr, &interrupted);

    if (!generated)
    {
        QTextStream(stderr) << "error: " << errorMessage << Qt::endl;
        return interrupted.load(std::memory_order_relaxed) ? 130 : 1;
    }

    output.write("\n");
    output.flush();
    QTextStream(stderr) << "model_load_ms=" << loadMilliseconds << Qt::endl;
    return 0;
}
