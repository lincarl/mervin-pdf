// Model fixtures adapted from Tesseract 5.5.4's matrix_deserializesize,
// weightmatrix_intdim, weightmatrix_scales, fullyconnected, lstm_layer,
// plumbing, lstm_nullchar and lstm_recoder tests, under Apache-2.0.
// https://github.com/tesseract-ocr/tesseract/tree/5.5.4/unittest
// Licensed under the Apache License, Version 2.0. You may obtain a copy at
// https://www.apache.org/licenses/LICENSE-2.0

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <tesseract/baseapi.h>
#include <mupdf/fitz.h>
extern "C" {
#include "leptonica-wrap.h"
}
#include "matrix.h"
#include "network.h"
#include "networkio.h"
#include "networkscratch.h"
#include "serialis.h"
#include "tessdatamanager.h"
#include "weightmatrix.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace tesseract;

namespace {

/// Builds the little-endian model format without allocating attacker-sized data.
class ModelBytes
{
public:
    std::vector<char> data;
    void byte(uint8_t value) { data.push_back(static_cast<char>(value)); }
    void integer(int32_t value)
    {
        const auto bits = static_cast<uint32_t>(value);
        for (int i = 0; i < 4; ++i)
            byte(static_cast<uint8_t>(bits >> (8 * i)));
    }
    void real(double value)
    {
        const auto bits = std::bit_cast<uint64_t>(value);
        for (int i = 0; i < 8; ++i)
            byte(static_cast<uint8_t>(bits >> (8 * i)));
    }
    void header(NetworkType type, int inputs, int outputs)
    {
        byte(type);
        byte(0); // Training disabled.
        byte(0); // No backpropagation.
        integer(0); // Network flags.
        integer(inputs);
        integer(outputs);
        integer(0); // Weight count.
        integer(0); // Empty name.
    }
    void weights(int rows, int columns)
    {
        byte(128); // Current float-mode matrix, serialized as doubles.
        integer(rows);
        integer(columns);
        real(0); // Empty cell.
        for (int i = 0; i < rows * columns; ++i)
            real(0);
    }
};

/// Reads a component from the pinned model, preserving its serialized bytes.
std::vector<char> component(TessdataManager &manager, TessdataType type)
{
    TFile file;
    if (!manager.GetComponent(type, &file))
        return {};
    std::vector<char> bytes(file.RemainingBytes());
    if (file.FRead(bytes.data(), 1, bytes.size()) != bytes.size())
        return {};
    return bytes;
}

/// Finds a non-space recoder code to change without tripping the space check.
size_t recoderCodeOffset(const std::vector<char> &bytes, int outputs)
{
    TFile file;
    if (!file.Open(bytes.data(), bytes.size()))
        return bytes.size();
    uint32_t entries = 0;
    if (!file.DeSerialize(&entries) || entries > bytes.size())
        return bytes.size();
    for (uint32_t entry = 0; entry < entries; ++entry) {
        uint8_t normalized = 0;
        uint32_t length = 0;
        if (!file.DeSerialize(&normalized) || !file.DeSerialize(&length)
            || length > file.RemainingBytes() / sizeof(int32_t))
            return bytes.size();
        for (uint32_t i = 0; i < length; ++i) {
            const size_t offset = bytes.size() - file.RemainingBytes();
            int32_t code = 0;
            if (!file.DeSerialize(&code))
                return bytes.size();
            if (entry > 0 && code >= 0 && code < outputs)
                return offset;
        }
    }
    return bytes.size();
}

} // namespace

/// Exercises the actual Tesseract objects linked into MuPDF on both platforms.
class TstTesseractModels : public QObject
{
    Q_OBJECT
    fz_context *context = nullptr;
private slots:
    void initTestCase();
    void cleanupTestCase();
    void runtimeVersion();
    void matrixDimensions_data();
    void matrixDimensions();
    void integerWeights_data();
    void integerWeights();
    void layerDimensions_data();
    void layerDimensions();
    void plumbing_data();
    void plumbing();
    void traineddata_data();
    void traineddata();
};

/// Match MuPDF's allocation setup before calling its bundled Tesseract directly.
void TstTesseractModels::initTestCase()
{
    context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    QVERIFY(context);
    fz_set_leptonica_mem(context);
}

/// Release the adapter's process-local allocation state after all model objects.
void TstTesseractModels::cleanupTestCase()
{
    fz_clear_leptonica_mem(context);
    fz_drop_context(context);
}

/// Checks the library's compiled version, rather than only the test headers.
void TstTesseractModels::runtimeVersion()
{
    QCOMPARE(QString::fromLatin1(TessBaseAPI::Version()), QStringLiteral("5.5.4"));
}

void TstTesseractModels::matrixDimensions_data()
{
    QTest::addColumn<int>("rows");
    QTest::addColumn<int>("columns");
    QTest::addColumn<int>("cells");
    QTest::addColumn<bool>("valid");
    QTest::newRow("valid") << 2 << 3 << 6 << true;
    QTest::newRow("valid boundary dimension") << 65535 << 1 << 65535 << true;
    QTest::newRow("negative dimensions") << -1 << -1 << 1 << false;
    QTest::newRow("negative second dimension") << 3 << -1 << 0 << false;
    QTest::newRow("overflowing product") << 65535 << 65535 << 0 << false;
    QTest::newRow("oversized dimension") << 65536 << 1 << 0 << false;
    QTest::newRow("unbacked allocation") << 32768 << 65535 << 8 << false;
}

void TstTesseractModels::matrixDimensions()
{
    QFETCH(int, rows);
    QFETCH(int, columns);
    QFETCH(int, cells);
    QFETCH(bool, valid);
    ModelBytes bytes;
    bytes.integer(rows);
    bytes.integer(columns);
    bytes.byte(0);
    bytes.data.resize(bytes.data.size() + cells, 0);
    TFile file;
    QVERIFY(file.Open(bytes.data.data(), bytes.data.size()));
    GENERIC_2D_ARRAY<int8_t> matrix;
    QCOMPARE(matrix.DeSerialize(&file), valid);
    if (valid) {
        QCOMPARE(matrix.dim1(), rows);
        QCOMPARE(matrix.dim2(), columns);
        QCOMPARE(file.RemainingBytes(), size_t(0));
    }
}

void TstTesseractModels::integerWeights_data()
{
    QTest::addColumn<int>("rows");
    QTest::addColumn<int>("columns");
    QTest::addColumn<int>("scales");
    QTest::addColumn<int>("storedScales");
    QTest::addColumn<bool>("legacy");
    QTest::addColumn<bool>("valid");
    QTest::newRow("valid") << 2 << 2 << 2 << 2 << false << true;
    QTest::newRow("valid SIMD padding") << 2 << 2 << 4 << 4 << false << true;
    QTest::newRow("zero outputs") << 0 << 2 << 0 << 0 << false << false;
    QTest::newRow("zero inputs") << 2 << 0 << 0 << 0 << false << false;
    QTest::newRow("no scales") << 2 << 2 << 0 << 0 << false << false;
    QTest::newRow("short scales") << 2 << 2 << 1 << 1 << false << false;
    QTest::newRow("unbacked scales") << 2 << 2 << 100000000 << 0 << false << false;
    // Artifex uses double precision, so the legacy float format remains supported.
    QTest::newRow("valid legacy") << 2 << 2 << 2 << 2 << true << true;
    QTest::newRow("legacy short scales") << 2 << 2 << 1 << 1 << true << false;
}

void TstTesseractModels::integerWeights()
{
    QFETCH(int, rows);
    QFETCH(int, columns);
    QFETCH(int, scales);
    QFETCH(int, storedScales);
    QFETCH(bool, legacy);
    QFETCH(bool, valid);
    ModelBytes bytes;
    bytes.byte(legacy ? 1 : 129);
    bytes.integer(rows);
    bytes.integer(columns);
    bytes.byte(0);
    bytes.data.resize(bytes.data.size() + rows * columns, 0);
    bytes.integer(scales);
    for (int i = 0; i < storedScales; ++i) {
        if (legacy)
            bytes.integer(std::bit_cast<int32_t>(1.0f));
        else
            bytes.real(1);
    }
    TFile file;
    QVERIFY(file.Open(bytes.data.data(), bytes.data.size()));
    WeightMatrix matrix;
    QCOMPARE(matrix.DeSerialize(false, &file), valid);
    if (valid)
        QCOMPARE(file.RemainingBytes(), size_t(0));
}

void TstTesseractModels::layerDimensions_data()
{
    QTest::addColumn<bool>("lstm");
    QTest::addColumn<int>("activationSize");
    QTest::addColumn<int>("rows");
    QTest::addColumn<int>("columns");
    QTest::addColumn<bool>("valid");
    QTest::newRow("valid softmax") << false << 0 << 2 << 2 << true;
    QTest::newRow("inconsistent softmax") << false << 0 << 3 << 5 << false;
    QTest::newRow("valid LSTM") << true << 6 << 5 << 7 << true;
    QTest::newRow("inconsistent activation buffer") << true << 2 << 5 << 7 << false;
    QTest::newRow("inconsistent gate rows") << true << 6 << 9 << 7 << false;
    QTest::newRow("inconsistent gate columns") << true << 6 << 5 << 9 << false;
}

void TstTesseractModels::layerDimensions()
{
    QFETCH(bool, lstm);
    QFETCH(int, activationSize);
    QFETCH(int, rows);
    QFETCH(int, columns);
    QFETCH(bool, valid);
    ModelBytes bytes;
    bytes.header(lstm ? NT_LSTM : NT_SOFTMAX, 1, lstm ? 5 : 2);
    if (lstm) {
        bytes.integer(activationSize);
        for (int gate = 0; gate < 4; ++gate)
            bytes.weights(gate == 1 ? rows : 5, gate == 1 ? columns : 7);
    } else {
        bytes.weights(rows, columns);
    }
    TFile file;
    QVERIFY(file.Open(bytes.data.data(), bytes.data.size()));
    std::unique_ptr<Network> network(Network::CreateFromFile(&file));
    QCOMPARE(bool(network), valid);
    if (network) {
        QCOMPARE(file.RemainingBytes(), size_t(0));
        StrideMap stride;
        stride.SetStride({{1, 2}});
        NetworkIO input;
        input.ResizeToMap(false, stride, 1);
        NetworkScratch scratch;
        NetworkIO output;
        network->Forward(false, input, nullptr, &scratch, &output);
        QCOMPARE(output.NumFeatures(), lstm ? 5 : 2);
    }
}

void TstTesseractModels::plumbing_data()
{
    QTest::addColumn<int>("type");
    QTest::addColumn<int>("children");
    QTest::addColumn<bool>("valid");
    QTest::newRow("empty series") << int(NT_SERIES) << 0 << false;
    QTest::newRow("empty parallel") << int(NT_PARALLEL) << 0 << false;
    QTest::newRow("empty reversed") << int(NT_XREVERSED) << 0 << false;
    QTest::newRow("short series") << int(NT_SERIES) << 1 << false;
    QTest::newRow("valid series") << int(NT_SERIES) << 2 << true;
}

void TstTesseractModels::plumbing()
{
    QFETCH(int, type);
    QFETCH(int, children);
    QFETCH(bool, valid);
    ModelBytes bytes;
    bytes.header(static_cast<NetworkType>(type), 1, 1);
    bytes.integer(children);
    for (int child = 0; child < children; ++child) {
        bytes.header(NT_INPUT, 1, 1);
        for (int dimension = 0; dimension < 4; ++dimension)
            bytes.integer(1);
        bytes.integer(0); // Loss type.
    }
    TFile file;
    QVERIFY(file.Open(bytes.data.data(), bytes.data.size()));
    std::unique_ptr<Network> network(Network::CreateFromFile(&file));
    QCOMPARE(bool(network), valid);
}

void TstTesseractModels::traineddata_data()
{
    QTest::addColumn<QString>("mutation");
    QTest::newRow("valid model") << QString();
    QTest::newRow("negative null character") << QStringLiteral("negative");
    QTest::newRow("oversized null character") << QStringLiteral("oversized");
    QTest::newRow("recoder exceeds output count") << QStringLiteral("recoder");
}

/// Mutates valid internals while retaining a valid traineddata container.
void TstTesseractModels::traineddata()
{
    QFETCH(QString, mutation);
    const QString directory = qEnvironmentVariable("MERVIN_TEST_TESSDATA_DIR",
                                                   QStringLiteral(MERVIN_TEST_TESSDATA_DIR));
    TessdataManager manager;
    QVERIFY2(manager.Init(QFile::encodeName(QDir(directory).filePath("eng.traineddata")).constData()),
             "Fetch the pinned test model with scripts/fetch-test-tessdata.py");
    QVERIFY(!manager.swap());
    QVERIFY(manager.IsComponentAvailable(TESSDATA_LSTM_UNICHARSET));
    QVERIFY(manager.IsComponentAvailable(TESSDATA_LSTM_RECODER));
    auto lstm = component(manager, TESSDATA_LSTM);
    QVERIFY(!lstm.empty());
    TFile networkFile;
    QVERIFY(networkFile.Open(lstm.data(), lstm.size()));
    std::unique_ptr<Network> network(Network::CreateFromFile(&networkFile));
    QVERIFY(network);
    const int32_t outputs = network->NumOutputs();
    QVERIFY(outputs > 0);
    if (mutation == "recoder") {
        auto recoder = component(manager, TESSDATA_LSTM_RECODER);
        const size_t offset = recoderCodeOffset(recoder, outputs);
        QVERIFY(offset + sizeof(outputs) <= recoder.size());
        std::memcpy(recoder.data() + offset, &outputs, sizeof(outputs));
        manager.OverwriteEntry(TESSDATA_LSTM_RECODER, recoder.data(), int(recoder.size()));
    } else if (!mutation.isEmpty()) {
        std::string description;
        int32_t value = 0;
        QVERIFY(networkFile.DeSerialize(description));
        for (int field = 0; field < 3; ++field)
            QVERIFY(networkFile.DeSerialize(&value));
        const size_t offset = lstm.size() - networkFile.RemainingBytes();
        QVERIFY(networkFile.DeSerialize(&value));
        QVERIFY(value >= 0 && value < outputs);
        value = mutation == "negative" ? -1 : outputs;
        std::memcpy(lstm.data() + offset, &value, sizeof(value));
        manager.OverwriteEntry(TESSDATA_LSTM, lstm.data(), int(lstm.size()));
    }
    QTemporaryDir models;
    QVERIFY(models.isValid());
    QVERIFY(manager.SaveFile(QFile::encodeName(models.filePath("eng.traineddata")).constData(), nullptr));
    TessBaseAPI api;
    QCOMPARE(api.Init(QFile::encodeName(models.path()).constData(), "eng", OEM_LSTM_ONLY),
             mutation.isEmpty() ? 0 : -1);
}

QTEST_GUILESS_MAIN(TstTesseractModels)
#include "tst_tesseract_models.moc"
