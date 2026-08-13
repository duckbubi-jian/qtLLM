#include "LlamaEngine.hpp"

#include <llama.h>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace qtllm::worker
{
namespace
{
struct ModelDeleter
{
    void operator()(llama_model* model) const
    {
        llama_model_free(model);
    }
};

struct ContextDeleter
{
    void operator()(llama_context* context) const
    {
        llama_free(context);
    }
};

struct SamplerDeleter
{
    void operator()(llama_sampler* sampler) const
    {
        llama_sampler_free(sampler);
    }
};

using ModelPointer = std::unique_ptr<llama_model, ModelDeleter>;
using ContextPointer = std::unique_ptr<llama_context, ContextDeleter>;
using SamplerPointer = std::unique_ptr<llama_sampler, SamplerDeleter>;

struct CancellationState
{
    const std::atomic_bool* internal = nullptr;
    const std::atomic_bool* external = nullptr;
};

bool shouldAbort(void* userData)
{
    const auto* state = static_cast<const CancellationState*>(userData);
    return state->internal->load(std::memory_order_relaxed) ||
           (state->external != nullptr &&
            state->external->load(std::memory_order_relaxed));
}

void logErrors(ggml_log_level level, const char* text, void*)
{
    if (level >= GGML_LOG_LEVEL_ERROR)
    {
        std::fputs(text, stderr);
        std::fflush(stderr);
    }
}

bool formatPrompt(const llama_model* model, const WorkerOptions& options,
                  QByteArray& prompt, QString& errorMessage)
{
    const auto systemPrompt = options.systemPrompt.toUtf8();
    const auto userPrompt = options.prompt.toUtf8();

    std::vector<llama_chat_message> messages;
    if (!systemPrompt.isEmpty())
    {
        messages.push_back({"system", systemPrompt.constData()});
    }
    messages.push_back({"user", userPrompt.constData()});

    const char* chatTemplate = llama_model_chat_template(model, nullptr);
    if (chatTemplate == nullptr)
    {
        errorMessage =
            QStringLiteral("The model has no chat template metadata.");
        return false;
    }

    const auto required = llama_chat_apply_template(
        chatTemplate, messages.data(), messages.size(), true, nullptr, 0);
    if (required < 0)
    {
        errorMessage =
            QStringLiteral("The model chat template is not supported.");
        return false;
    }

    prompt.resize(required + 1);
    const auto written = llama_chat_apply_template(
        chatTemplate, messages.data(), messages.size(), true, prompt.data(),
        prompt.size());
    if (written < 0 || written > required)
    {
        errorMessage =
            QStringLiteral("Failed to apply the model chat template.");
        return false;
    }

    prompt.resize(written);
    return true;
}

bool tokenize(const llama_vocab* vocabulary, const QByteArray& prompt,
              std::vector<llama_token>& tokens, QString& errorMessage)
{
    const auto required = -llama_tokenize(
        vocabulary, prompt.constData(), prompt.size(), nullptr, 0, true, true);
    if (required <= 0)
    {
        errorMessage =
            QStringLiteral("Failed to determine prompt token count.");
        return false;
    }

    tokens.resize(required);
    const auto written =
        llama_tokenize(vocabulary, prompt.constData(), prompt.size(),
                       tokens.data(), tokens.size(), true, true);
    if (written < 0)
    {
        errorMessage = QStringLiteral("Failed to tokenize the prompt.");
        return false;
    }

    tokens.resize(written);
    return true;
}

bool tokenPiece(const llama_vocab* vocabulary, llama_token token,
                QByteArray& piece, QString& errorMessage)
{
    std::vector<char> buffer(256);
    auto written = llama_token_to_piece(vocabulary, token, buffer.data(),
                                        buffer.size(), 0, true);
    if (written < 0)
    {
        buffer.resize(-written);
        written = llama_token_to_piece(vocabulary, token, buffer.data(),
                                       buffer.size(), 0, true);
    }
    if (written < 0)
    {
        errorMessage =
            QStringLiteral("Failed to convert a generated token to UTF-8.");
        return false;
    }

    piece = QByteArray(buffer.data(), written);
    return true;
}
}  // namespace

LlamaEngine::LlamaEngine()
{
    llama_log_set(logErrors, nullptr);
    llama_backend_init();
    ggml_backend_load_all();
}

LlamaEngine::~LlamaEngine()
{
    llama_backend_free();
}

bool LlamaEngine::generate(const WorkerOptions& options,
                           const TokenHandler& tokenHandler,
                           QString& errorMessage,
                           const std::atomic_bool* externalCancellation)
{
    cancelled_.store(false, std::memory_order_relaxed);
    CancellationState cancellationState{&cancelled_, externalCancellation};

    QElapsedTimer loadTimer;
    loadTimer.start();

    auto modelParameters = llama_model_default_params();
    modelParameters.n_gpu_layers = options.gpuLayers;
    modelParameters.use_mmap = true;

    const auto modelPath = options.modelPath.toUtf8();
    ModelPointer model(
        llama_model_load_from_file(modelPath.constData(), modelParameters));
    if (!model)
    {
        errorMessage = QStringLiteral("Unable to load GGUF model: %1")
                           .arg(QFileInfo(options.modelPath).fileName());
        return false;
    }

    const auto loadMilliseconds = loadTimer.elapsed();
    const auto* vocabulary = llama_model_get_vocab(model.get());
    if (vocabulary == nullptr)
    {
        errorMessage = QStringLiteral("The loaded model has no vocabulary.");
        return false;
    }

    QByteArray formattedPrompt;
    if (!formatPrompt(model.get(), options, formattedPrompt, errorMessage))
    {
        return false;
    }

    std::vector<llama_token> promptTokens;
    if (!tokenize(vocabulary, formattedPrompt, promptTokens, errorMessage))
    {
        return false;
    }

    if (promptTokens.size() + static_cast<std::size_t>(options.maxTokens) >
        static_cast<std::size_t>(options.contextSize))
    {
        errorMessage = QStringLiteral(
                           "Prompt (%1 tokens) plus --max-tokens (%2) exceeds "
                           "--context-size (%3).")
                           .arg(promptTokens.size())
                           .arg(options.maxTokens)
                           .arg(options.contextSize);
        return false;
    }

    auto contextParameters = llama_context_default_params();
    contextParameters.n_ctx = options.contextSize;
    contextParameters.n_batch =
        std::min<std::uint32_t>(options.contextSize, 2048U);
    contextParameters.n_threads = options.threads;
    contextParameters.n_threads_batch = options.threads;
    contextParameters.abort_callback = shouldAbort;
    contextParameters.abort_callback_data = &cancellationState;
    contextParameters.no_perf = false;

    ContextPointer context(
        llama_init_from_model(model.get(), contextParameters));
    if (!context)
    {
        errorMessage =
            QStringLiteral("Unable to create the inference context.");
        return false;
    }

    auto samplerParameters = llama_sampler_chain_default_params();
    samplerParameters.no_perf = false;
    SamplerPointer sampler(llama_sampler_chain_init(samplerParameters));
    if (!sampler)
    {
        errorMessage = QStringLiteral("Unable to create the sampler chain.");
        return false;
    }

    llama_sampler_chain_add(
        sampler.get(),
        llama_sampler_init_penalties(-1, options.repeatPenalty, 0.0F, 0.0F));
    llama_sampler_chain_add(sampler.get(),
                            llama_sampler_init_top_k(options.topK));
    llama_sampler_chain_add(sampler.get(),
                            llama_sampler_init_top_p(options.topP, 1));
    llama_sampler_chain_add(sampler.get(),
                            llama_sampler_init_temp(options.temperature));
    llama_sampler_chain_add(sampler.get(),
                            llama_sampler_init_dist(options.seed));

    QElapsedTimer responseTimer;
    responseTimer.start();

    constexpr int32_t promptBatchSize = 2048;
    std::size_t promptOffset = 0;
    while (promptOffset < promptTokens.size())
    {
        const auto remaining = promptTokens.size() - promptOffset;
        const auto batchSize = static_cast<int32_t>(
            std::min<std::size_t>(remaining, promptBatchSize));
        auto promptBatch =
            llama_batch_get_one(promptTokens.data() + promptOffset, batchSize);
        const auto decodeResult = llama_decode(context.get(), promptBatch);
        if (decodeResult != 0)
        {
            if (shouldAbort(&cancellationState))
            {
                errorMessage = QStringLiteral("Generation cancelled.");
            }
            else
            {
                errorMessage =
                    QStringLiteral("Prompt decode failed with code %1.")
                        .arg(decodeResult);
            }
            return false;
        }
        promptOffset += static_cast<std::size_t>(batchSize);
    }
    const auto promptEvaluationMilliseconds = responseTimer.elapsed();

    QElapsedTimer generationTimer;
    generationTimer.start();

    int generatedTokens = 0;
    qint64 firstTokenMilliseconds = -1;
    while (generatedTokens < options.maxTokens)
    {
        if (shouldAbort(&cancellationState))
        {
            errorMessage = QStringLiteral("Generation cancelled.");
            return false;
        }

        auto token = llama_sampler_sample(sampler.get(), context.get(), -1);
        if (llama_vocab_is_eog(vocabulary, token))
        {
            break;
        }

        QByteArray piece;
        if (!tokenPiece(vocabulary, token, piece, errorMessage))
        {
            return false;
        }
        tokenHandler(piece);
        if (firstTokenMilliseconds < 0)
        {
            firstTokenMilliseconds = responseTimer.elapsed();
        }
        ++generatedTokens;
        if (generatedTokens == options.maxTokens)
        {
            break;
        }

        auto tokenBatch = llama_batch_get_one(&token, 1);
        const auto decodeResult = llama_decode(context.get(), tokenBatch);
        if (decodeResult != 0)
        {
            if (shouldAbort(&cancellationState))
            {
                errorMessage = QStringLiteral("Generation cancelled.");
            }
            else
            {
                errorMessage =
                    QStringLiteral("Model decode failed with code %1.")
                        .arg(decodeResult);
            }
            return false;
        }
    }

    const auto generationSeconds = generationTimer.elapsed() / 1000.0;
    const auto tokensPerSecond =
        generationSeconds > 0.0 ? generatedTokens / generationSeconds : 0.0;
    QTextStream(stderr) << "model_load_ms=" << loadMilliseconds
                        << " prompt_eval_ms=" << promptEvaluationMilliseconds
                        << " first_token_ms=" << firstTokenMilliseconds
                        << " prompt_tokens=" << promptTokens.size()
                        << " generated_tokens=" << generatedTokens
                        << " generation_tokens_per_second="
                        << QString::number(tokensPerSecond, 'f', 2) << Qt::endl;

    return true;
}

void LlamaEngine::cancel()
{
    cancelled_.store(true, std::memory_order_relaxed);
}
}  // namespace qtllm::worker
