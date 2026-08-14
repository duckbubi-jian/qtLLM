#include "Logging.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string_view>
#include <vector>

namespace qtllm::logging
{
namespace
{
QMutex stateMutex;
QString activeLogDirectory;
QString activeLogFilePath;

QString safeComponentName(QStringView component)
{
    QString safe;
    safe.reserve(component.size());
    for (const auto character : component)
    {
        safe.append(character.isLetterOrNumber() || character == u'-' ||
                            character == u'_'
                        ? character
                        : QLatin1Char('_'));
    }
    return safe.isEmpty() ? QStringLiteral("qtLLM") : safe;
}

QString defaultLogDirectory()
{
    const auto configured = qEnvironmentVariable("QTLLM_LOG_DIR").trimmed();
    if (!configured.isEmpty()) return QDir::cleanPath(configured);

    auto dataDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dataDirectory.isEmpty())
        dataDirectory = QCoreApplication::applicationDirPath();
    return QDir(dataDirectory).filePath(QStringLiteral("logs"));
}

spdlog::filename_t nativeFileName(const QString& path)
{
#ifdef SPDLOG_WCHAR_FILENAMES
    return path.toStdWString();
#else
    return path.toStdString();
#endif
}

void write(spdlog::level::level_enum level, QStringView message)
{
    try
    {
        const auto utf8 = message.toUtf8();
        spdlog::log(level,
                    std::string_view(utf8.constData(),
                                     static_cast<std::size_t>(utf8.size())));
    }
    catch (...)
    {
    }
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context,
                      const QString& message)
{
    QString formatted = message;
    if (context.file != nullptr && context.line > 0)
    {
        formatted += QStringLiteral(" (%1:%2)")
                         .arg(QString::fromUtf8(context.file))
                         .arg(context.line);
    }

    switch (type)
    {
        case QtDebugMsg:
            debug(formatted);
            break;
        case QtInfoMsg:
            info(formatted);
            break;
        case QtWarningMsg:
            warning(formatted);
            break;
        case QtCriticalMsg:
            error(formatted);
            break;
        case QtFatalMsg:
            critical(formatted);
            break;
    }
}
}  // namespace

bool initialize(QStringView component, QString* errorMessage)
{
    const QMutexLocker locker(&stateMutex);
    const auto safeComponent = safeComponentName(component);
    const auto directory = defaultLogDirectory();
    const auto filePath =
        QDir(directory).filePath(safeComponent + QStringLiteral(".log"));

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());

    QString initializationError;
    if (!QDir().mkpath(directory))
    {
        initializationError =
            QStringLiteral("Unable to create log directory: %1").arg(directory);
    }
    else
    {
        try
        {
            constexpr std::size_t maximumLogBytes = 5U * 1024U * 1024U;
            constexpr std::size_t rotatedLogFiles = 3U;
            sinks.push_back(
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    nativeFileName(filePath), maximumLogBytes,
                    rotatedLogFiles));
        }
        catch (const spdlog::spdlog_ex& exception)
        {
            initializationError =
                QStringLiteral("Unable to open log file %1: %2")
                    .arg(filePath, QString::fromUtf8(exception.what()));
        }
    }

    try
    {
        auto logger = std::make_shared<spdlog::logger>(
            safeComponent.toStdString(), sinks.cbegin(), sinks.cend());
        logger->set_level(spdlog::level::debug);
        logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] [pid %P tid %t] %v");
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(logger));
    }
    catch (const spdlog::spdlog_ex& exception)
    {
        initializationError = QString::fromUtf8(exception.what());
    }

    activeLogDirectory = directory;
    activeLogFilePath = initializationError.isEmpty() ? filePath : QString{};
    if (errorMessage != nullptr) *errorMessage = initializationError;
    return initializationError.isEmpty();
}

void installQtMessageHandler()
{
    qInstallMessageHandler(qtMessageHandler);
}

void shutdown()
{
    qInstallMessageHandler(nullptr);
    spdlog::shutdown();
    const QMutexLocker locker(&stateMutex);
    activeLogDirectory.clear();
    activeLogFilePath.clear();
}

QString logDirectory()
{
    const QMutexLocker locker(&stateMutex);
    return activeLogDirectory;
}

QString logFilePath()
{
    const QMutexLocker locker(&stateMutex);
    return activeLogFilePath;
}

void debug(QStringView message)
{
    write(spdlog::level::debug, message);
}

void info(QStringView message)
{
    write(spdlog::level::info, message);
}

void warning(QStringView message)
{
    write(spdlog::level::warn, message);
}

void error(QStringView message)
{
    write(spdlog::level::err, message);
}

void critical(QStringView message)
{
    write(spdlog::level::critical, message);
}
}  // namespace qtllm::logging
