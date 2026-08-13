#include "LlamaEngine.hpp"
#include "WorkerOptions.hpp"

#include "protocol/ProtocolVersion.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

#include <atomic>
#include <csignal>

namespace
{
std::atomic_bool interrupted = false;

void handleInterrupt(int)
{
    interrupted.store(true, std::memory_order_relaxed);
}
}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qtllm-worker"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral("0.1.0 (protocol %1)").arg(qtllm::protocol::version));

    QCommandLineParser parser;
    qtllm::worker::configureParser(parser);
    parser.process(application);

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
        errorMessage, &interrupted);

    if (!generated)
    {
        QTextStream(stderr) << "error: " << errorMessage << Qt::endl;
        return interrupted.load(std::memory_order_relaxed) ? 130 : 1;
    }

    output.write("\n");
    output.flush();
    return 0;
}
