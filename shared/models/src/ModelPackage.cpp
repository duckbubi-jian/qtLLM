#include "ModelPackage.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QVersionNumber>

#include <cmath>
#include <utility>

namespace qtllm::models
{
namespace
{
constexpr auto maximumMetadataSize = 1024 * 1024;
constexpr auto maximumPackageFiles = 1024;

ModelPackageResult failure(QString code, QString message,
                           ModelSelection selection = {})
{
    selection.descriptor.verificationStatus = VerificationStatus::Invalid;
    return {std::move(selection), std::move(code), std::move(message)};
}

bool readJsonObject(const QString& path, QJsonObject& object,
                    QString& errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        errorMessage = QStringLiteral("Unable to read %1: %2")
                           .arg(QFileInfo(path).fileName(), file.errorString());
        return false;
    }
    if (file.size() > maximumMetadataSize)
    {
        errorMessage = QStringLiteral("%1 exceeds the 1 MiB metadata limit.")
                           .arg(QFileInfo(path).fileName());
        return false;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
    {
        errorMessage =
            QStringLiteral("%1 is not valid JSON: %2")
                .arg(QFileInfo(path).fileName(), parseError.errorString());
        return false;
    }
    if (!document.isObject())
    {
        errorMessage = QStringLiteral("%1 must contain a JSON object.")
                           .arg(QFileInfo(path).fileName());
        return false;
    }
    object = document.object();
    return true;
}

bool readRequiredString(const QJsonObject& object, const QString& name,
                        QString& value, QString& errorMessage)
{
    const auto item = object.value(name);
    if (!item.isString() || item.toString().trimmed().isEmpty())
    {
        errorMessage =
            QStringLiteral("%1 must be a non-empty string.").arg(name);
        return false;
    }
    value = item.toString().trimmed();
    return true;
}

template <typename Value>
bool readRequiredInteger(const QJsonObject& object, const QString& name,
                         Value minimum, Value maximum, Value& value,
                         QString& errorMessage)
{
    const auto item = object.value(name);
    if (!item.isDouble())
    {
        errorMessage = QStringLiteral("%1 must be an integer.").arg(name);
        return false;
    }
    const auto number = item.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum))
    {
        errorMessage = QStringLiteral("%1 is out of range.").arg(name);
        return false;
    }
    value = static_cast<Value>(number);
    return true;
}

bool readRequiredFloat(const QJsonObject& object, const QString& name,
                       float minimum, float maximum, float& value,
                       QString& errorMessage)
{
    const auto item = object.value(name);
    if (!item.isDouble() || !std::isfinite(item.toDouble()) ||
        item.toDouble() < minimum || item.toDouble() > maximum)
    {
        errorMessage = QStringLiteral("%1 is out of range.").arg(name);
        return false;
    }
    value = static_cast<float>(item.toDouble());
    return true;
}

bool normalizeRelativePath(const QString& value, QString& normalized,
                           QString& errorMessage)
{
    const auto portable = QDir::fromNativeSeparators(value.trimmed());
    normalized = QDir::cleanPath(portable);
    if (portable.isEmpty() || QDir::isAbsolutePath(portable) ||
        normalized == QLatin1String("..") ||
        normalized.startsWith(QLatin1String("../")) ||
        normalized == QLatin1String("."))
    {
        errorMessage = QStringLiteral(
                           "Package path is not a safe relative "
                           "path: %1")
                           .arg(value);
        return false;
    }
    return true;
}

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString comparablePath(QString path)
{
    path = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    path = path.toLower();
#endif
    return path;
}

bool isInsidePackage(const QString& packagePath, const QString& candidatePath)
{
    const auto package = comparablePath(packagePath);
    const auto candidate = comparablePath(candidatePath);
    return candidate == package ||
           candidate.startsWith(package + QLatin1Char('/'),
                                pathCaseSensitivity());
}

bool resolvePackageFile(const QString& packagePath, const QString& relativePath,
                        QString& absolutePath, QString& errorMessage)
{
    absolutePath = QDir(packagePath).absoluteFilePath(relativePath);
    const QFileInfo info(absolutePath);
    if (!info.exists() || !info.isFile())
    {
        errorMessage =
            QStringLiteral("Package file does not exist: %1").arg(relativePath);
        return false;
    }
    const auto canonicalPath = info.canonicalFilePath();
    if (canonicalPath.isEmpty() || !isInsidePackage(packagePath, canonicalPath))
    {
        errorMessage = QStringLiteral(
                           "Package file resolves outside the "
                           "package directory: %1")
                           .arg(relativePath);
        return false;
    }
    absolutePath = canonicalPath;
    return true;
}

bool hasGgufHeader(const QString& path, QString& errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        errorMessage = QStringLiteral("Unable to read model file %1: %2")
                           .arg(QFileInfo(path).fileName(), file.errorString());
        return false;
    }
    if (file.read(4) != QByteArrayLiteral("GGUF"))
    {
        errorMessage = QStringLiteral(
                           "Model file does not have a GGUF header: "
                           "%1")
                           .arg(QFileInfo(path).fileName());
        return false;
    }
    return true;
}

bool validatePackageContents(const QString& packagePath,
                             const QSet<QString>& declaredModels,
                             QString& errorMessage)
{
    static const QSet<QString> executableSuffixes{
        QStringLiteral("bat"), QStringLiteral("cmd"),   QStringLiteral("com"),
        QStringLiteral("dll"), QStringLiteral("dylib"), QStringLiteral("exe"),
        QStringLiteral("js"),  QStringLiteral("jse"),   QStringLiteral("msi"),
        QStringLiteral("msp"), QStringLiteral("pif"),   QStringLiteral("ps1"),
        QStringLiteral("py"),  QStringLiteral("pyw"),   QStringLiteral("scr"),
        QStringLiteral("sh"),  QStringLiteral("so"),    QStringLiteral("vbe"),
        QStringLiteral("vbs"), QStringLiteral("wsf"),   QStringLiteral("wsh")};

    auto fileCount = 0;
    QDirIterator iterator(packagePath,
                          QDir::Files | QDir::Dirs | QDir::Hidden |
                              QDir::System | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext())
    {
        iterator.next();
        if (++fileCount > maximumPackageFiles)
        {
            errorMessage =
                QStringLiteral("Model package exceeds the %1 file limit.")
                    .arg(maximumPackageFiles);
            return false;
        }
        const auto info = iterator.fileInfo();
        if (info.isSymLink() &&
            !isInsidePackage(packagePath, info.canonicalFilePath()))
        {
            errorMessage =
                QStringLiteral("Package link resolves outside the package: %1")
                    .arg(QDir(packagePath)
                             .relativeFilePath(info.absoluteFilePath()));
            return false;
        }
        if (executableSuffixes.contains(info.suffix().toLower()))
        {
            errorMessage =
                QStringLiteral(
                    "Executable content is not allowed in a model "
                    "package: %1")
                    .arg(QDir(packagePath)
                             .relativeFilePath(info.absoluteFilePath()));
            return false;
        }
        if (info.isFile() && info.suffix().compare(QStringLiteral("gguf"),
                                                   Qt::CaseInsensitive) == 0)
        {
            const auto relative = comparablePath(
                QDir(packagePath).relativeFilePath(info.absoluteFilePath()));
            if (!declaredModels.contains(relative))
            {
                errorMessage =
                    QStringLiteral(
                        "GGUF file is not declared in manifest.json: "
                        "%1")
                        .arg(relative);
                return false;
            }
        }
    }
    return true;
}

bool parsePreset(const QString& path, InferencePreset& preset,
                 QString& errorMessage)
{
    QJsonObject object;
    return readJsonObject(path, object, errorMessage) &&
           readRequiredInteger(object, QStringLiteral("contextSize"), 256,
                               1'048'576, preset.contextSize, errorMessage) &&
           readRequiredInteger(object, QStringLiteral("maxOutputTokens"), 1,
                               1'048'576, preset.maxOutputTokens,
                               errorMessage) &&
           readRequiredFloat(object, QStringLiteral("temperature"), 0.0F, 10.0F,
                             preset.temperature, errorMessage) &&
           readRequiredFloat(object, QStringLiteral("topP"), 0.0F, 1.0F,
                             preset.topP, errorMessage) &&
           readRequiredInteger(object, QStringLiteral("topK"), 0, 1'000'000,
                               preset.topK, errorMessage) &&
           readRequiredFloat(object, QStringLiteral("repeatPenalty"), 0.0F,
                             10.0F, preset.repeatPenalty, errorMessage);
}

ModelPackageResult inspectDirectGguf(const QFileInfo& modelInfo)
{
    ModelSelection selection;
    selection.selectedPath = modelInfo.absoluteFilePath();
    selection.modelPath = modelInfo.absoluteFilePath();
    selection.descriptor.displayName = modelInfo.completeBaseName();
    selection.descriptor.format = QStringLiteral("gguf");
    selection.descriptor.engine = QStringLiteral("llama.cpp");
    selection.descriptor.packageDirectory = modelInfo.absolutePath();
    selection.descriptor.origin = ModelOrigin::DirectGguf;
    selection.descriptor.verificationStatus = VerificationStatus::NotVerified;
    selection.descriptor.modelFiles.append(
        {modelInfo.fileName(), modelInfo.size(), {}});

    if (modelInfo.suffix().compare(QStringLiteral("gguf"),
                                   Qt::CaseInsensitive) != 0)
        return failure(QStringLiteral("unsupported_model_file"),
                       QStringLiteral("Direct model files must use the .gguf "
                                      "extension."),
                       std::move(selection));

    QString errorMessage;
    if (!hasGgufHeader(modelInfo.absoluteFilePath(), errorMessage))
        return failure(QStringLiteral("invalid_gguf"), errorMessage,
                       std::move(selection));
    return {std::move(selection), {}, {}};
}
}  // namespace

ModelPackageResult ModelPackage::inspect(const QString& selectedPath,
                                         const QString& runtimeVersion)
{
    const QFileInfo selectedInfo(selectedPath.trimmed());
    if (!selectedInfo.exists())
        return failure(
            QStringLiteral("model_path_not_found"),
            QStringLiteral("Model path does not exist: %1").arg(selectedPath));
    if (selectedInfo.isFile()) return inspectDirectGguf(selectedInfo);
    if (!selectedInfo.isDir())
        return failure(QStringLiteral("invalid_model_path"),
                       QStringLiteral("Model path is neither a file nor a "
                                      "directory: %1")
                           .arg(selectedPath));

    ModelSelection selection;
    selection.selectedPath = selectedInfo.absoluteFilePath();
    selection.descriptor.packageDirectory = selectedInfo.canonicalFilePath();
    selection.descriptor.origin = ModelOrigin::ExternalPackage;
    selection.descriptor.verificationStatus = VerificationStatus::NotVerified;
    if (selection.descriptor.packageDirectory.isEmpty())
        return failure(QStringLiteral("invalid_model_package"),
                       QStringLiteral("Unable to resolve model package path."),
                       std::move(selection));

    const auto manifestPath = QDir(selection.descriptor.packageDirectory)
                                  .filePath(QStringLiteral("manifest.json"));
    QJsonObject manifest;
    QString errorMessage;
    if (!readJsonObject(manifestPath, manifest, errorMessage))
        return failure(QStringLiteral("invalid_manifest"), errorMessage,
                       std::move(selection));

    auto& descriptor = selection.descriptor;
    if (!readRequiredInteger(manifest, QStringLiteral("schemaVersion"), 1, 1,
                             descriptor.schemaVersion, errorMessage) ||
        !readRequiredString(manifest, QStringLiteral("id"), descriptor.id,
                            errorMessage) ||
        !readRequiredString(manifest, QStringLiteral("displayName"),
                            descriptor.displayName, errorMessage) ||
        !readRequiredString(manifest, QStringLiteral("packageVersion"),
                            descriptor.packageVersion, errorMessage) ||
        !readRequiredString(manifest, QStringLiteral("modelRevision"),
                            descriptor.modelRevision, errorMessage) ||
        !readRequiredString(manifest, QStringLiteral("engine"),
                            descriptor.engine, errorMessage) ||
        !readRequiredString(manifest, QStringLiteral("format"),
                            descriptor.format, errorMessage) ||
        !readRequiredString(manifest, QStringLiteral("minimumRuntimeVersion"),
                            descriptor.minimumRuntimeVersion, errorMessage) ||
        !readRequiredInteger(manifest, QStringLiteral("recommendedRamGb"), 1,
                             4096, descriptor.recommendedRamGb, errorMessage) ||
        !readRequiredInteger(manifest, QStringLiteral("defaultContextSize"),
                             256, 1'048'576, descriptor.defaultContextSize,
                             errorMessage) ||
        !readRequiredString(manifest, QStringLiteral("presetFile"),
                            descriptor.presetFile, errorMessage) ||
        !readRequiredString(manifest, QStringLiteral("licenseFile"),
                            descriptor.licenseFile, errorMessage))
        return failure(QStringLiteral("invalid_manifest"), errorMessage,
                       std::move(selection));

    static const QRegularExpression idPattern(
        QStringLiteral("^[a-z0-9][a-z0-9._-]*$"));
    if (!idPattern.match(descriptor.id).hasMatch())
        return failure(QStringLiteral("invalid_manifest"),
                       QStringLiteral("id must contain only lowercase ASCII "
                                      "letters, numbers, dots, underscores, "
                                      "and hyphens."),
                       std::move(selection));
    if (descriptor.engine != QLatin1String("llama.cpp") ||
        descriptor.format.compare(QStringLiteral("gguf"),
                                  Qt::CaseInsensitive) != 0)
        return failure(QStringLiteral("unsupported_model_package"),
                       QStringLiteral("Only llama.cpp GGUF model packages are "
                                      "supported."),
                       std::move(selection));

    const auto requiredVersion =
        QVersionNumber::fromString(descriptor.minimumRuntimeVersion);
    const auto currentVersion = QVersionNumber::fromString(runtimeVersion);
    static const QRegularExpression versionPattern(
        QStringLiteral("^[0-9]+(?:\\.[0-9]+)*$"));
    if (!versionPattern.match(descriptor.minimumRuntimeVersion).hasMatch() ||
        !versionPattern.match(runtimeVersion).hasMatch() ||
        requiredVersion.isNull() || currentVersion.isNull())
        return failure(
            QStringLiteral("invalid_runtime_version"),
            QStringLiteral("Runtime versions must use numeric dotted "
                           "version syntax."),
            std::move(selection));
    if (QVersionNumber::compare(currentVersion, requiredVersion) < 0)
        return failure(
            QStringLiteral("runtime_too_old"),
            QStringLiteral("Model package requires runtime %1 or newer; this "
                           "runtime is %2.")
                .arg(descriptor.minimumRuntimeVersion, runtimeVersion),
            std::move(selection));

    const auto modelFilesValue = manifest.value(QStringLiteral("modelFiles"));
    if (!modelFilesValue.isArray() || modelFilesValue.toArray().isEmpty() ||
        modelFilesValue.toArray().size() > 128)
        return failure(QStringLiteral("invalid_manifest"),
                       QStringLiteral("modelFiles must contain between 1 and "
                                      "128 files."),
                       std::move(selection));

    static const QRegularExpression sha256Pattern(
        QStringLiteral("^[0-9a-f]{64}$"));
    QSet<QString> declaredModels;
    for (const auto& value : modelFilesValue.toArray())
    {
        if (!value.isObject())
            return failure(QStringLiteral("invalid_manifest"),
                           QStringLiteral("Each modelFiles entry must be an "
                                          "object."),
                           std::move(selection));
        ModelFileDescriptor modelFile;
        const auto object = value.toObject();
        if (!readRequiredString(object, QStringLiteral("path"), modelFile.path,
                                errorMessage) ||
            !readRequiredInteger(object, QStringLiteral("sizeBytes"), qint64{1},
                                 qint64{9'007'199'254'740'991},
                                 modelFile.sizeBytes, errorMessage) ||
            !readRequiredString(object, QStringLiteral("sha256"),
                                modelFile.sha256, errorMessage))
            return failure(QStringLiteral("invalid_manifest"), errorMessage,
                           std::move(selection));
        if (!normalizeRelativePath(modelFile.path, modelFile.path,
                                   errorMessage))
            return failure(QStringLiteral("unsafe_package_path"), errorMessage,
                           std::move(selection));
        if (!modelFile.path.endsWith(QStringLiteral(".gguf"),
                                     Qt::CaseInsensitive) ||
            !sha256Pattern.match(modelFile.sha256).hasMatch())
            return failure(
                QStringLiteral("invalid_manifest"),
                QStringLiteral("Each model file must be a .gguf file "
                               "with a lowercase SHA-256 value."),
                std::move(selection));
        const auto comparable = comparablePath(modelFile.path);
        if (declaredModels.contains(comparable))
            return failure(QStringLiteral("invalid_manifest"),
                           QStringLiteral("modelFiles contains a duplicate "
                                          "path: %1")
                               .arg(modelFile.path),
                           std::move(selection));
        declaredModels.insert(comparable);

        QString absolutePath;
        if (!resolvePackageFile(descriptor.packageDirectory, modelFile.path,
                                absolutePath, errorMessage))
            return failure(QStringLiteral("invalid_model_package"),
                           errorMessage, std::move(selection));
        const QFileInfo fileInfo(absolutePath);
        if (fileInfo.size() != modelFile.sizeBytes)
            return failure(
                QStringLiteral("model_size_mismatch"),
                QStringLiteral("Model file size does not match manifest.json: "
                               "%1 (expected %2, found %3 bytes)")
                    .arg(modelFile.path)
                    .arg(modelFile.sizeBytes)
                    .arg(fileInfo.size()),
                std::move(selection));
        if (!hasGgufHeader(absolutePath, errorMessage))
            return failure(QStringLiteral("invalid_gguf"), errorMessage,
                           std::move(selection));
        if (selection.modelPath.isEmpty()) selection.modelPath = absolutePath;
        descriptor.modelFiles.append(std::move(modelFile));
    }

    QString normalizedPreset;
    QString normalizedLicense;
    if (!normalizeRelativePath(descriptor.presetFile, normalizedPreset,
                               errorMessage) ||
        !normalizeRelativePath(descriptor.licenseFile, normalizedLicense,
                               errorMessage))
        return failure(QStringLiteral("unsafe_package_path"), errorMessage,
                       std::move(selection));
    descriptor.presetFile = normalizedPreset;
    descriptor.licenseFile = normalizedLicense;

    QString presetPath;
    QString licensePath;
    if (!resolvePackageFile(descriptor.packageDirectory, descriptor.presetFile,
                            presetPath, errorMessage) ||
        !resolvePackageFile(descriptor.packageDirectory, descriptor.licenseFile,
                            licensePath, errorMessage))
        return failure(QStringLiteral("invalid_model_package"), errorMessage,
                       std::move(selection));
    if (QFileInfo(licensePath).size() <= 0)
        return failure(QStringLiteral("invalid_model_package"),
                       QStringLiteral("The package license file is empty."),
                       std::move(selection));
    if (!parsePreset(presetPath, selection.preset, errorMessage))
        return failure(QStringLiteral("invalid_preset"), errorMessage,
                       std::move(selection));
    if (selection.preset.contextSize != descriptor.defaultContextSize)
        return failure(QStringLiteral("invalid_preset"),
                       QStringLiteral("preset.json contextSize must match "
                                      "manifest.json defaultContextSize."),
                       std::move(selection));
    if (selection.preset.maxOutputTokens >= selection.preset.contextSize)
        return failure(QStringLiteral("invalid_preset"),
                       QStringLiteral("maxOutputTokens must be smaller than "
                                      "contextSize."),
                       std::move(selection));

    const auto upstreamValue = manifest.value(QStringLiteral("upstream"));
    if (!upstreamValue.isObject())
        return failure(QStringLiteral("invalid_manifest"),
                       QStringLiteral("upstream must be an object."),
                       std::move(selection));
    if (!readRequiredString(upstreamValue.toObject(),
                            QStringLiteral("publisher"),
                            descriptor.upstream.publisher, errorMessage) ||
        !readRequiredString(upstreamValue.toObject(), QStringLiteral("modelId"),
                            descriptor.upstream.modelId, errorMessage) ||
        !readRequiredString(upstreamValue.toObject(),
                            QStringLiteral("revision"),
                            descriptor.upstream.revision, errorMessage) ||
        !readRequiredString(upstreamValue.toObject(), QStringLiteral("source"),
                            descriptor.upstream.source, errorMessage))
        return failure(QStringLiteral("invalid_manifest"), errorMessage,
                       std::move(selection));

    if (!validatePackageContents(descriptor.packageDirectory, declaredModels,
                                 errorMessage))
        return failure(QStringLiteral("invalid_model_package"), errorMessage,
                       std::move(selection));
    return {std::move(selection), {}, {}};
}

ModelPackageResult ModelPackage::verify(ModelSelection selection)
{
    if (selection.descriptor.origin == ModelOrigin::DirectGguf)
        return {std::move(selection), {}, {}};

    for (const auto& modelFile : selection.descriptor.modelFiles)
    {
        QString absolutePath;
        QString errorMessage;
        if (!resolvePackageFile(selection.descriptor.packageDirectory,
                                modelFile.path, absolutePath, errorMessage))
            return failure(QStringLiteral("invalid_model_package"),
                           errorMessage, std::move(selection));

        QFile file(absolutePath);
        if (!file.open(QIODevice::ReadOnly))
            return failure(QStringLiteral("model_hash_failed"),
                           QStringLiteral("Unable to hash %1: %2")
                               .arg(modelFile.path, file.errorString()),
                           std::move(selection));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file))
            return failure(QStringLiteral("model_hash_failed"),
                           QStringLiteral("Unable to read %1 while computing "
                                          "SHA-256.")
                               .arg(modelFile.path),
                           std::move(selection));
        const auto actual = QString::fromLatin1(hash.result().toHex());
        if (actual != modelFile.sha256)
            return failure(
                QStringLiteral("model_hash_mismatch"),
                QStringLiteral("SHA-256 does not match manifest.json for %1 "
                               "(expected %2, found %3).")
                    .arg(modelFile.path, modelFile.sha256, actual),
                std::move(selection));
    }
    selection.descriptor.verificationStatus = VerificationStatus::Verified;
    return {std::move(selection), {}, {}};
}
}  // namespace qtllm::models
