#include "ModelPackage.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace qtllm::tests
{
namespace
{
bool writeFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(contents) == contents.size();
}

QJsonObject validPreset()
{
    return {{QStringLiteral("contextSize"), 4096},
            {QStringLiteral("maxOutputTokens"), 512},
            {QStringLiteral("temperature"), 0.6},
            {QStringLiteral("topP"), 0.95},
            {QStringLiteral("topK"), 40},
            {QStringLiteral("repeatPenalty"), 1.05}};
}

QJsonObject validManifest(
    const QByteArray& model,
    const QString& minimumRuntimeVersion = QStringLiteral("0.1.0"))
{
    const auto hash = QString::fromLatin1(
        QCryptographicHash::hash(model, QCryptographicHash::Sha256).toHex());
    return {
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("id"), QStringLiteral("test-model-q4km")},
        {QStringLiteral("displayName"), QStringLiteral("Test Model Q4_K_M")},
        {QStringLiteral("packageVersion"), QStringLiteral("1.0.0")},
        {QStringLiteral("modelRevision"), QStringLiteral("fixed-revision")},
        {QStringLiteral("engine"), QStringLiteral("llama.cpp")},
        {QStringLiteral("format"), QStringLiteral("gguf")},
        {QStringLiteral("modelFiles"),
         QJsonArray{
             QJsonObject{{QStringLiteral("path"), QStringLiteral("model.gguf")},
                         {QStringLiteral("sizeBytes"), model.size()},
                         {QStringLiteral("sha256"), hash}}}},
        {QStringLiteral("minimumRuntimeVersion"), minimumRuntimeVersion},
        {QStringLiteral("recommendedRamGb"), 8},
        {QStringLiteral("defaultContextSize"), 4096},
        {QStringLiteral("presetFile"), QStringLiteral("preset.json")},
        {QStringLiteral("licenseFile"), QStringLiteral("LICENSE.txt")},
        {QStringLiteral("upstream"),
         QJsonObject{
             {QStringLiteral("publisher"), QStringLiteral("Test")},
             {QStringLiteral("modelId"), QStringLiteral("test/model")},
             {QStringLiteral("revision"), QStringLiteral("fixed-revision")},
             {QStringLiteral("source"),
              QStringLiteral("https://example.test/model")}}}};
}

bool writePackage(const QString& directory, const QByteArray& model,
                  const QJsonObject& manifest,
                  const QJsonObject& preset = validPreset())
{
    return writeFile(QDir(directory).filePath(QStringLiteral("model.gguf")),
                     model) &&
           writeFile(QDir(directory).filePath(QStringLiteral("manifest.json")),
                     QJsonDocument(manifest).toJson()) &&
           writeFile(QDir(directory).filePath(QStringLiteral("preset.json")),
                     QJsonDocument(preset).toJson()) &&
           writeFile(QDir(directory).filePath(QStringLiteral("LICENSE.txt")),
                     QByteArrayLiteral("Test license\n"));
}
}  // namespace

class ModelPackageTest final : public QObject
{
    Q_OBJECT

   private slots:
    void inspectsAndVerifiesValidPackage();
    void detectsHashMismatch();
    void rejectsPathTraversal();
    void rejectsExecutableContent();
    void rejectsUndeclaredGguf();
    void acceptsSplitModelPackage();
    void rejectsMismatchedPreset();
    void rejectsNewerRuntimeRequirement();
    void upgradesPackageGgufSelectionToPackage();
    void rejectsBrokenAdjacentManifest();
    void acceptsDirectGgufAsUnverified();
    void verifiesExternalPackageWhenConfigured();
};

void ModelPackageTest::inspectsAndVerifiesValidPackage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto model = QByteArrayLiteral("GGUF-test-model");
    QVERIFY(writePackage(directory.path(), model, validManifest(model)));

    auto result = models::ModelPackage::inspect(directory.path(),
                                                QStringLiteral("0.1.0"));
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.selection.descriptor.id, QStringLiteral("test-model-q4km"));
    QCOMPARE(result.selection.descriptor.verificationStatus,
             models::VerificationStatus::NotVerified);
    QCOMPARE(result.selection.preset.contextSize, 4096);
    QCOMPARE(result.selection.preset.maxOutputTokens, 512);
    QCOMPARE(result.selection.modelPath,
             QDir(directory.path()).filePath(QStringLiteral("model.gguf")));

    quint64 verifiedBytes = 0;
    quint64 totalBytes = 0;
    auto progressUpdates = 0;
    result = models::ModelPackage::verify(std::move(result.selection),
                                          [&](quint64 verified, quint64 total)
                                          {
                                              verifiedBytes = verified;
                                              totalBytes = total;
                                              ++progressUpdates;
                                          });
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.selection.descriptor.verificationStatus,
             models::VerificationStatus::Verified);
    QCOMPARE(verifiedBytes, static_cast<quint64>(model.size()));
    QCOMPARE(totalBytes, static_cast<quint64>(model.size()));
    QVERIFY(progressUpdates >= 2);
}

void ModelPackageTest::detectsHashMismatch()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto model = QByteArrayLiteral("GGUF-test-model");
    auto manifest = validManifest(model);
    auto files = manifest.value(QStringLiteral("modelFiles")).toArray();
    auto file = files.at(0).toObject();
    file.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0')));
    files.replace(0, file);
    manifest.insert(QStringLiteral("modelFiles"), files);
    QVERIFY(writePackage(directory.path(), model, manifest));

    auto result = models::ModelPackage::inspect(directory.path(),
                                                QStringLiteral("0.1.0"));
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    result = models::ModelPackage::verify(std::move(result.selection));
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorCode, QStringLiteral("model_hash_mismatch"));
}

void ModelPackageTest::rejectsPathTraversal()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto model = QByteArrayLiteral("GGUF-test-model");
    auto manifest = validManifest(model);
    auto files = manifest.value(QStringLiteral("modelFiles")).toArray();
    auto file = files.at(0).toObject();
    file.insert(QStringLiteral("path"), QStringLiteral("../model.gguf"));
    files.replace(0, file);
    manifest.insert(QStringLiteral("modelFiles"), files);
    QVERIFY(writePackage(directory.path(), model, manifest));

    const auto result = models::ModelPackage::inspect(directory.path(),
                                                      QStringLiteral("0.1.0"));
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorCode, QStringLiteral("unsafe_package_path"));
}

void ModelPackageTest::rejectsExecutableContent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto model = QByteArrayLiteral("GGUF-test-model");
    QVERIFY(writePackage(directory.path(), model, validManifest(model)));
    QVERIFY(
        writeFile(QDir(directory.path()).filePath(QStringLiteral("setup.exe")),
                  QByteArrayLiteral("not executable")));

    const auto result = models::ModelPackage::inspect(directory.path(),
                                                      QStringLiteral("0.1.0"));
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorCode, QStringLiteral("invalid_model_package"));
}

void ModelPackageTest::rejectsUndeclaredGguf()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto model = QByteArrayLiteral("GGUF-test-model");
    QVERIFY(writePackage(directory.path(), model, validManifest(model)));
    QVERIFY(writeFile(
        QDir(directory.path()).filePath(QStringLiteral("second-model.gguf")),
        QByteArrayLiteral("GGUF-second-model")));

    const auto result = models::ModelPackage::inspect(directory.path(),
                                                      QStringLiteral("0.1.0"));
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorCode, QStringLiteral("invalid_model_package"));
}

void ModelPackageTest::acceptsSplitModelPackage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto firstShard = QByteArrayLiteral("GGUF-test-model-shard-one");
    const auto secondShard = QByteArrayLiteral("GGUF-test-model-shard-two");
    auto manifest = validManifest(firstShard);
    auto files = manifest.value(QStringLiteral("modelFiles")).toArray();
    auto firstFile = files.at(0).toObject();
    firstFile.insert(QStringLiteral("path"),
                     QStringLiteral("model-00001-of-00002.gguf"));
    files.replace(0, firstFile);
    const auto secondHash = QString::fromLatin1(
        QCryptographicHash::hash(secondShard, QCryptographicHash::Sha256)
            .toHex());
    const QJsonObject secondFile{
        {QStringLiteral("path"), QStringLiteral("model-00002-of-00002.gguf")},
        {QStringLiteral("sizeBytes"), secondShard.size()},
        {QStringLiteral("sha256"), secondHash}};
    files.append(secondFile);
    manifest.insert(QStringLiteral("modelFiles"), files);
    QVERIFY(
        writeFile(QDir(directory.path())
                      .filePath(QStringLiteral("model-00001-of-00002.gguf")),
                  firstShard));
    QVERIFY(
        writeFile(QDir(directory.path())
                      .filePath(QStringLiteral("model-00002-of-00002.gguf")),
                  secondShard));
    QVERIFY(writeFile(
        QDir(directory.path()).filePath(QStringLiteral("manifest.json")),
        QJsonDocument(manifest).toJson()));
    QVERIFY(writeFile(
        QDir(directory.path()).filePath(QStringLiteral("preset.json")),
        QJsonDocument(validPreset()).toJson()));
    QVERIFY(writeFile(
        QDir(directory.path()).filePath(QStringLiteral("LICENSE.txt")),
        QByteArrayLiteral("Test license\n")));

    auto result = models::ModelPackage::inspect(directory.path(),
                                                QStringLiteral("0.1.0"));
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.selection.descriptor.modelFiles.size(), 2);
    QCOMPARE(result.selection.modelPath,
             QDir(directory.path())
                 .filePath(QStringLiteral("model-00001-of-00002.gguf")));

    result = models::ModelPackage::verify(std::move(result.selection));
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.selection.descriptor.verificationStatus,
             models::VerificationStatus::Verified);
}

void ModelPackageTest::rejectsMismatchedPreset()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto model = QByteArrayLiteral("GGUF-test-model");
    auto preset = validPreset();
    preset.insert(QStringLiteral("contextSize"), 8192);
    QVERIFY(
        writePackage(directory.path(), model, validManifest(model), preset));

    const auto result = models::ModelPackage::inspect(directory.path(),
                                                      QStringLiteral("0.1.0"));
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorCode, QStringLiteral("invalid_preset"));
}

void ModelPackageTest::rejectsNewerRuntimeRequirement()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto model = QByteArrayLiteral("GGUF-test-model");
    QVERIFY(writePackage(directory.path(), model,
                         validManifest(model, QStringLiteral("9.0.0"))));

    const auto result = models::ModelPackage::inspect(directory.path(),
                                                      QStringLiteral("0.1.0"));
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorCode, QStringLiteral("runtime_too_old"));
}

void ModelPackageTest::upgradesPackageGgufSelectionToPackage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto model = QByteArrayLiteral("GGUF-test-model");
    QVERIFY(writePackage(directory.path(), model, validManifest(model)));
    const auto modelPath =
        QDir(directory.path()).filePath(QStringLiteral("model.gguf"));

    auto result =
        models::ModelPackage::inspect(modelPath, QStringLiteral("0.1.0"));
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.selection.descriptor.origin,
             models::ModelOrigin::ExternalPackage);
    QCOMPARE(result.selection.selectedPath,
             QFileInfo(directory.path()).absoluteFilePath());
    QCOMPARE(result.selection.descriptor.displayName,
             QStringLiteral("Test Model Q4_K_M"));

    result = models::ModelPackage::verify(std::move(result.selection));
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.selection.descriptor.verificationStatus,
             models::VerificationStatus::Verified);
}

void ModelPackageTest::rejectsBrokenAdjacentManifest()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto modelPath =
        QDir(directory.path()).filePath(QStringLiteral("model.gguf"));
    QVERIFY(writeFile(modelPath, QByteArrayLiteral("GGUF-test-model")));
    QVERIFY(writeFile(
        QDir(directory.path()).filePath(QStringLiteral("manifest.json")),
        QByteArrayLiteral("{}")));

    const auto result =
        models::ModelPackage::inspect(modelPath, QStringLiteral("0.1.0"));
    QVERIFY(!result.succeeded());
    QCOMPARE(result.errorCode, QStringLiteral("invalid_manifest"));
}

void ModelPackageTest::acceptsDirectGgufAsUnverified()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path =
        QDir(directory.path()).filePath(QStringLiteral("local.gguf"));
    QVERIFY(writeFile(path, QByteArrayLiteral("GGUF-local-model")));

    auto result = models::ModelPackage::inspect(path, QStringLiteral("0.1.0"));
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.selection.descriptor.origin,
             models::ModelOrigin::DirectGguf);
    QCOMPARE(result.selection.descriptor.verificationStatus,
             models::VerificationStatus::NotVerified);
    result = models::ModelPackage::verify(std::move(result.selection));
    QVERIFY(result.succeeded());
    QCOMPARE(result.selection.descriptor.verificationStatus,
             models::VerificationStatus::NotVerified);
}

void ModelPackageTest::verifiesExternalPackageWhenConfigured()
{
    const auto packagePath =
        QString::fromLocal8Bit(qgetenv("QTLLM_TEST_MODEL_PACKAGE"));
    if (packagePath.isEmpty())
        QSKIP("Set QTLLM_TEST_MODEL_PACKAGE to verify a real model package.");

    auto result =
        models::ModelPackage::inspect(packagePath, QStringLiteral("0.1.0"));
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    result = models::ModelPackage::verify(std::move(result.selection));
    QVERIFY2(result.succeeded(), qPrintable(result.errorMessage));
    QCOMPARE(result.selection.descriptor.verificationStatus,
             models::VerificationStatus::Verified);
}
}  // namespace qtllm::tests

QTEST_GUILESS_MAIN(qtllm::tests::ModelPackageTest)

#include "ModelPackageTest.moc"
