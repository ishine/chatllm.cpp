#include "chat.h"
#include <iomanip>
#include <iostream>
#include <fstream>
#include <thread>
#include <algorithm>
#include <numeric>
#include <functional>
#include <cstring>
#include <random>
#include <map>

#include "vectorstore.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

struct Args
{
    std::string model_path = "";
    std::string embedding_model_path = "";
    std::string reranker_model_path = "";
    std::vector<std::string> vector_store;
    std::string vector_store_in = "";
    std::string system = "";
    std::string prompt = "你好";
    std::string extending = "restart";
    std::string test_fn = "";
    std::string rag_template = "";
    std::string rag_context_sep = "";
    std::map<std::string, std::string> additional;
    int max_length = -1;
    int max_context_length = 512;
    bool interactive = false;
    int top_k = 0;
    float top_p = 0.7;
    float temp = 0.7;
    int num_threads = 0;
    bool multi_line = false;
    int seed;
    chatllm::ChatFormat format = chatllm::ChatFormat::CHAT;
    bool tokenize = false;
    DistanceStrategy vc = DistanceStrategy::MaxInnerProduct;
    int retrieve_top_n = 2;
    int rerank_top_n = 1;
    float rerank_score_thres = 0.35;
    int rag_post_extending = 0;
    bool hide_reference = false;
    bool rag_dump = false;
    bool show_banner = true;
};

#define MULTI_LINE_END_MARKER_W  L"\\."
#define MULTI_LINE_END_MARKER     "\\."

void usage(const char *prog)
{
    std::cout << "Usage: " << prog << " [options]\n"
              << "\n"
              << "Basic options:\n"
              << "  -h, --help              show this help message and exit\n"
              << "  -m, --model PATH        model path\n"
              << "  -p, --prompt PROMPT     prompt to start generation with (default: 你好)\n"
              << "  -s, --system SYSTEM     system prompt (instruction) (default: model specific)\n"
              << "  -i, --interactive       run in interactive mode\n"
              << "  -l, --max_length N      max total length including prompt and output (default: model specific)\n"
              << "                          generally, this is used to reduce KV cache size.\n"
              << "                          for models that does not show its max context window in `config.json`,\n"
              << "                          use this to enlarge it (use with caution!).\n"
              << "  -n, --threads N         number of threads for inference (default: number of cores)\n"
              << "  -c, --max_context_length N\n"
              << "                          max context length (default: 512)\n"
              << "  --extending EXT         context extending method (EXT = restart | shift) (default: restart)\n"
              << "  --multi                 enabled multiple lines of input\n"
              << "                          when enabled,  `" << MULTI_LINE_END_MARKER << "` marks the end of your input.\n"
              << "  --format FMT            conversion format (model specific, FMT = chat | completion | qa) (default: chat)\n"
              << "Sampling options:\n"
              << "  -t, --temp T            temperature (default: 0.7)\n"
              << "  --top_k N               top-k sampling (default: 0)\n"
              << "  --top_p N               top-p sampling (default: 0.7)\n"
              << "  --seed N                seed for random generator (default: random)\n"
              << "RAG options:\n"
              << "  --vector_store FILE     append a vector store file (when at lease one is specifed, RAG is enabled)\n"
              << "  --embedding_model PATH  embedding model path (mandatory if RAG is enabled)\n"
              << "  --distance_strategy DS  distance strategy (model dependent, default: MaxInnerProduct)\n"
              << "                          DS = EuclideanDistance | MaxInnerProduct | InnerProduct | CosineSimilarity\n"
              << "  --retrieve_top_n N      number of retrieved items using embedding model (default: 2)\n"
              << "  --reranker_model PATH   reranker model path (optional)\n"
              << "  --rerank_score_thres    reranking score threshold (default: 0.35)\n"
              << "                          items with a lower score are discarded.\n"
              << "  --rerank_top_n N        number of selected items using reranker model (default: 1)\n"
              << "  --hide_reference        do not show references (default: false)\n"
              << "  --rag_template ...      prompt template for RAG (macros: {context}, {question}) (optional).\n"
              << "                          Support some C escape sequences (\\n). Example:\n"
              << "                          Answer the question according to below information:\n"
              << "                          ---\n"
              << "                          {context}\n"
              << "                          ---\n"
              << "                          Question: {question}\n"
              << "  --rag_context_sep       context separator (default: '\\n```\\n')\n"
              << "                          Support some C escape sequences (\\n).\n"
              << "  --rag_post_extending N  extend selected items with pre & post N chunks with same metadata. (default: 0)\n"
              << "                          this may be useful when context length of embedding/reranker models is limited.\n"
              << "   +rag_dump              (debug) dump retrieved/re-ranking results\n"
              << "Misc:\n"
              << "  --init_vs FILE          init vector store file from input\n"
              << "  --tokenize              (debug) tokenize `prompt` and exit\n"
              << "  --test FILE             test against inputs from a file and exit\n"
              << "  --hide_banner           hide banner\n"
              << "Additional key-value args:\n"
              << "  --kv                    start of additional args. following options are interpreted as k-v pairs\n"
              << "  key value               a key-value pair of args\n"
              << std::endl;
}

static Args parse_args(int argc, const char **argv)
{
    Args args;
    std::random_device rd;
    args.seed = rd();

    #define handle_para0(fmt1, field, f)    \
        else if ((strcmp(arg, fmt1) == 0))      \
        {                                                                   \
            c++;                                                            \
            if (c < argc)                                                   \
                args.field = f(argv[c]);                                    \
        }

    #define handle_param(fmt1, fmt2, field, f)    \
        else if ((strcmp(arg, fmt1) == 0) || (strcmp(arg, fmt2) == 0))      \
        {                                                                   \
            c++;                                                            \
            if (c < argc)                                                   \
                args.field = f(argv[c]);                                    \
        }

    #define append_param(fmt1, field, f)    \
        else if ((strcmp(arg, fmt1) == 0))      \
        {                                                                   \
            c++;                                                            \
            if (c < argc)                                                   \
                args.field.push_back(f(argv[c]));                           \
        }

    int c = 1;
    while (c < argc)
    {
        const char *arg = argv[c];
        if ((strcmp(arg, "--help") == 0) || (strcmp(arg, "-h") == 0))
        {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        else if ((strcmp(arg, "--interactive") == 0) || (strcmp(arg, "-i") == 0))
        {
            args.interactive = true;
        }
        else if (strcmp(arg, "--multi") == 0)
        {
            args.multi_line = true;
        }
        else if (strcmp(arg, "--tokenize") == 0)
        {
            args.tokenize = true;
        }
        else if (strcmp(arg, "--hide_reference") == 0)
        {
            args.hide_reference = true;
        }
        else if (strcmp(arg, "--hide_banner") == 0)
        {
            args.show_banner = false;
        }
        else if (strcmp(arg, "+rag_dump") == 0)
        {
            args.rag_dump = true;
        }
        else if (strcmp(arg, "--format") == 0)
        {
            c++;
            if (c < argc)
            {
                if (strcmp(argv[c], "completion") == 0)
                    args.format = chatllm::ChatFormat::COMPLETION;
                else if (strcmp(argv[c], "qa") == 0)
                    args.format = chatllm::ChatFormat::QA;
                else
                    args.format = chatllm::ChatFormat::CHAT;
            }
        }
        else if (strcmp(arg, "--kv") == 0)
        {
            while (c + 2 < argc)
            {
                args.additional.insert_or_assign(argv[c + 1], argv[c + 2]);
                c += 2;
            }
        }
        handle_param("--model",                 "-m", model_path,           std::string)
        handle_param("--prompt",                "-p", prompt,               std::string)
        handle_param("--system",                "-s", system,               std::string)
        handle_param("--max_length",            "-l", max_length,           std::stoi)
        handle_param("--max_context_length",    "-c", max_context_length,   std::stoi)
        handle_para0("--extending",                   extending,            std::string)
        handle_param("--top_k",                 "-k", top_k,                std::stoi)
        handle_param("--top_p",                 "-q", top_p,                std::stof)
        handle_param("--temp",                  "-t", temp,                 std::stof)
        handle_param("--threads",               "-n", num_threads,          std::stoi)
        handle_para0("--seed",                        seed,                 std::stoi)
        handle_para0("--test",                        test_fn,              std::string)
        append_param("--vector_store",                vector_store,         std::string)
        handle_para0("--embedding_model",             embedding_model_path, std::string)
        handle_para0("--distance_strategy",           vc,                   ParseDistanceStrategy)
        handle_para0("--retrieve_top_n",              retrieve_top_n,       std::stoi)
        handle_para0("--reranker_model",              reranker_model_path,  std::string)
        handle_para0("--rerank_score_thres",          rerank_score_thres,   std::stof)
        handle_para0("--rerank_top_n",                rerank_top_n,         std::stoi)
        handle_para0("--rag_post_extending",          rag_post_extending,   std::stoi)
        handle_para0("--rag_template",                rag_template,         std::string)
        handle_para0("--rag_context_sep",             rag_context_sep,      std::string)
        handle_para0("--init_vs",                     vector_store_in,      std::string)
        else
            break;

        c++;
    }

    if (c < argc)
    {
        std::cerr << "Unknown arguments:";
        for (int i = c; i < argc; i++)
        {
            std::cerr << " " << argv[i];
        }
        std::cerr << std::endl;
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    return args;
}

#if defined(_WIN32)
static void append_utf8(char32_t ch, std::string &out)
{
    if (ch <= 0x7F)
    {
        out.push_back(static_cast<unsigned char>(ch));
    }
    else if (ch <= 0x7FF)
    {
        out.push_back(static_cast<unsigned char>(0xC0 | ((ch >> 6) & 0x1F)));
        out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    }
    else if (ch <= 0xFFFF)
    {
        out.push_back(static_cast<unsigned char>(0xE0 | ((ch >> 12) & 0x0F)));
        out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    }
    else if (ch <= 0x10FFFF)
    {
        out.push_back(static_cast<unsigned char>(0xF0 | ((ch >> 18) & 0x07)));
        out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 12) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    }
    else
    {
        // Invalid Unicode code point
    }
}

static bool get_utf8_line(std::string &line, bool multi_line)
{
    std::wstring marker(MULTI_LINE_END_MARKER_W);

    do
    {
        std::wstring prompt;
        std::getline(std::wcin, prompt);

        if (multi_line)
        {
            if (prompt == marker)
                return true;
            if (line.size() > 0)
                append_utf8('\n', line);
        }

        for (auto wc : prompt)
            append_utf8(wc, line);
    } while (multi_line);

    return true;
}
#else
static bool get_utf8_line(std::string &line, bool multi_line)
{
    do
    {
        std::string prompt;
        std::getline(std::cin, prompt);

        if (multi_line)
        {
            if (prompt == MULTI_LINE_END_MARKER)
                return true;
            if (line.size() > 0)
                line.push_back('\n');
        }

        line.append(prompt.begin(), prompt.end());
    } while (multi_line);

    return true;
}
#endif

static inline int get_num_physical_cores()
{
    unsigned int n_threads = std::thread::hardware_concurrency();
    return n_threads > 0 ? (n_threads <= 4 ? n_threads : n_threads / 2) : 4;
}

static void trim(std::string &s)
{
    size_t l = s.size();
    while (l > 0)
    {
        if ((s[l - 1] == '\r') || (s[l - 1] == '\n'))
            l--;
        else
            break;
    }
    s.resize(l);
}

static void run_file(Args &args, chatllm::Pipeline &pipeline, chatllm::TextStreamer &streamer, const chatllm::GenerationConfig &gen_config)
{
    std::vector<std::string> history;
    std::string input;
    std::ifstream f(args.test_fn);

    if (f.is_open())
    {
        while (std::getline(f, input))
        {
            trim(input);
            std::cout << "You  > " << input << std::endl;
            history.emplace_back(std::move(input));

            std::cout << "A.I. > " << std::flush;
            std::string output = pipeline.chat(history, gen_config, &streamer);
            history.emplace_back(std::move(output));
        }
    }

    f.close();
    std::cout << std::endl << pipeline.model->get_n_past() << " tokens are processed/generated. Bye" << std::endl;
}

static void show_banner(chatllm::Pipeline &pipeline, bool show)
{
    if (!show) return;
    if (pipeline.is_loaded())
    {
        #define MODEL_INFO()     "You are served by " << std::left << std::setw(28) << pipeline.model->type_name() + ","
        #define SHOW_NATIVE()    if (pipeline.model->native_name().size() > 0) { std::cout << "(" << pipeline.model->native_name() << ")"; }

        const int64_t total_param_num = pipeline.model->get_param_num(false);
        const int64_t total_effective_param_num = pipeline.model->get_param_num(true);

        std::cout   << R"(    ________          __  __    __    __  ___ )"; SHOW_NATIVE(); std::cout << '\n'
                    << R"(   / ____/ /_  ____ _/ /_/ /   / /   /  |/  /_________  ____  )" << '\n'
                    << R"(  / /   / __ \/ __ `/ __/ /   / /   / /|_/ // ___/ __ \/ __ \ )" << '\n'
                    << R"( / /___/ / / / /_/ / /_/ /___/ /___/ /  / // /__/ /_/ / /_/ / )" << '\n'
                    << R"( \____/_/ /_/\__,_/\__/_____/_____/_/  /_(_)___/ .___/ .___/  )" << '\n';
        std::cout   << MODEL_INFO()                               << R"(/_/   /_/       )" << '\n';
        if (total_param_num == total_effective_param_num)
            std::cout   << "with " << total_param_num << " (" << std::fixed << std::setprecision(1) << total_param_num / 1000000000. << "B) parameters." << '\n';
        else
            std::cout   << "with " << total_param_num << " (" << std::fixed << std::setprecision(1) << total_effective_param_num / 1000000000. << "B effect.) parameters." << '\n';
    }
    else
    {
        std::cout   << R"(    ________          __  __    __    __  ___ )" << '\n'
                    << R"(   / ____/ /_  ____ _/ /_/ /   / /   /  |/  /_________  ____  )" << '\n'
                    << R"(  / /   / __ \/ __ `/ __/ /   / /   / /|_/ // ___/ __ \/ __ \ )" << '\n'
                    << R"( / /___/ / / / /_/ / /_/ /___/ /___/ /  / // /__/ /_/ / /_/ / )" << '\n'
                    << R"( \____/_/ /_/\__,_/\__/_____/_____/_/  /_(_)___/ .___/ .___/  )" << '\n';
        std::cout   << R"(No LLM is loaded.                             /_/   /_/       )" << '\n';
    }

    auto additional = pipeline.get_additional_description();
    if (additional.size() > 0)
    {
        std::cout << additional << std::endl;
    }

    std::cout << std::endl;
}

static void print_embedding(const std::vector<float> &data)
{
    for (size_t i = 0; i < data.size(); i++)
    {
        if ((i % 8) == 0) std::cout << std::endl;
        std::cout << std::setw(14) << std::fixed << std::setprecision(8) << data[i] << "  ";
    }
    std::cout << std::endl;
}

static void run_text_embedding(Args &args, chatllm::Pipeline &pipeline, chatllm::TextStreamer &streamer, const chatllm::GenerationConfig &gen_config)
{
    std::vector<float> result;

    if (!args.interactive)
    {
        pipeline.text_embedding(args.prompt, gen_config, result);
        print_embedding(result);
        return;
    }

    show_banner(pipeline, args.show_banner);

    while (1)
    {
        std::cout << "Input > " << std::flush;
        std::string input;
        if (!get_utf8_line(input, args.multi_line))
        {
            std::cout << "FAILED to read line." << std::endl;
            break;
        }
        if (input.empty()) continue;

        result.clear();
        pipeline.text_embedding(input, gen_config, result);
        std::cout << "      > ";

        print_embedding(result);

    }
    std::cout << "Bye\n";
}

static void run_qa_ranker(Args &args, chatllm::Pipeline &pipeline, chatllm::TextStreamer &streamer, const chatllm::GenerationConfig &gen_config)
{
    show_banner(pipeline, args.show_banner);

    while (1)
    {
        std::cout << "Answer > " << std::flush;
        std::string answer;
        if (!get_utf8_line(answer, args.multi_line))
        {
            std::cout << "FAILED to read line." << std::endl;
            break;
        }
        if (answer.empty()) continue;

        float rank = pipeline.qa_rank(args.prompt, answer, gen_config);
        std::cout << std::setw(14) << std::fixed << std::setprecision(8) << rank << std::endl;
    }
    std::cout << "Bye\n";
}

void chat(Args &args, chatllm::Pipeline &pipeline)
{
    if (args.system.size() > 0)
        pipeline.set_system_prompt(args.system);

    if (pipeline.is_loaded())
    {
        pipeline.model->seed(args.seed);
        args.max_length = pipeline.model->get_max_length();

        if (args.extending == "shift")
            pipeline.set_extending_method(chatllm::Pipeline::ExtendingMethod::Shift);
        else
            pipeline.set_extending_method(chatllm::Pipeline::ExtendingMethod::Restart);

        pipeline.tokenizer->set_chat_format(args.format);
    }

    if (args.tokenize)
    {
        auto ids = pipeline.tokenizer->encode(args.prompt);
        std::cout << "ID: ";
        for (auto x : ids)
            std::cout << x << ", ";
        std::cout << std::endl;
        return;
    }

    pipeline.set_additional_args(args.additional);

    const std::string ai_prompt   = "A.I.";
    const std::string user_prompt = "You";
    const int prompt_len = 4;

    chatllm::TextStreamer streamer(pipeline.tokenizer);

    chatllm::GenerationConfig gen_config(args.max_length, args.max_context_length, args.temp > 0, args.top_k,
                                         args.top_p, args.temp, args.num_threads);
    std::vector<std::string> history;

    if (pipeline.is_loaded())
    {
        switch (pipeline.model->get_purpose())
        {
        case chatllm::ModelPurpose::TextEmbedding:
            run_text_embedding(args, pipeline, streamer, gen_config);
            return;
        case chatllm::ModelPurpose::Ranker:
            run_qa_ranker(args, pipeline, streamer, gen_config);
            return;
        default:
            break;
        }
    }

    if (args.test_fn.size() > 0)
    {
        run_file(args, pipeline, streamer, gen_config);
        return;
    }

    if (!args.interactive)
    {
        history.push_back(args.prompt);
        pipeline.chat(history, gen_config, &streamer);
        return;
    }

    show_banner(pipeline, args.show_banner);

    while (1)
    {
        std::cout << std::setw(prompt_len) << std::left << user_prompt << " > " << std::flush;
        std::string input;
        if (!get_utf8_line(input, args.multi_line))
        {
            std::cout << "FAILED to read line." << std::endl;
            break;
        }
        if (input.empty()) continue;

        history.emplace_back(std::move(input));
        std::cout << std::setw(prompt_len) << std::left << ai_prompt << " > " << std::flush;
        std::string output = pipeline.chat(history, gen_config, &streamer);
        history.emplace_back(std::move(output));
    }
    std::cout << "Bye\n";
}

static int init_vector_store(Args &args)
{
    chatllm::Pipeline pipeline(args.embedding_model_path);
    args.max_length = pipeline.model->get_max_length();
    chatllm::GenerationConfig gen_config(args.max_length, args.max_context_length, args.temp > 0, args.top_k,
                                         args.top_p, args.temp, args.num_threads);
    std::vector<float> r;

    CVectorStore vs(args.vc, pipeline.get_text_embedding_dim(),
        [&pipeline, &gen_config, &r](const std::string &s, float *emb)
        {
            pipeline.text_embedding(s, gen_config, r);
            CHATLLM_CHECK((int)r.size() == pipeline.get_text_embedding_dim()) << "embedding dim mismatch";
            memcpy(emb, r.data(), r.size() * sizeof(float));
        },
        args.vector_store_in.c_str());
    vs.ExportDB((args.vector_store_in + ".vsdb").c_str());
    printf("Vector store saved to: %s\n", (args.vector_store_in + ".vsdb").c_str());
    return 0;
}

#if defined(_WIN32)
std::string wstr_to_utf8(const wchar_t* wstr)
{
    int s = WideCharToMultiByte(CP_UTF8, 0, wstr, (int)wcslen(wstr), NULL, 0, NULL, NULL);
    std::string str;
    str.resize(s);
    WideCharToMultiByte(CP_UTF8, 0, wstr, (int)wcslen(wstr), LPSTR(str.data()), s, NULL, NULL);
    return str;
}

int wmain(int argc, const wchar_t **wargv)
{
    std::vector<const char *> vect_args;
    std::vector<std::string> utf_args;
    for (int i = 0; i < argc; i++)
        utf_args.push_back(wstr_to_utf8(wargv[i]));
    for (int i = 0; i < argc; i++)
        vect_args.push_back(utf_args[i].data());
    const char **argv = vect_args.data();

    _setmode(_fileno(stdin), _O_WTEXT);
    // Set console code page to UTF-8 so console known how to interpret string data
    SetConsoleOutputCP(CP_UTF8);
    // Enable buffering to prevent VS from chopping up UTF-8 byte sequences
    setvbuf(stdout, nullptr, _IOFBF, 1000);

#else
int main(int argc, const char **argv)
{
#endif

    Args args = parse_args(argc, argv);
    if (args.num_threads <= 0)
        args.num_threads = get_num_physical_cores();

    if (args.vector_store_in.size() > 0)
        return init_vector_store(args);

    try
    {
        chatllm::ModelObject::extra_args pipe_args(args.max_length);
        if (args.embedding_model_path.size() < 1)
        {
            chatllm::Pipeline pipeline(args.model_path, pipe_args);
            chat(args, pipeline);
        }
        else
        {
            pipe_args.rag_dump = args.rag_dump;
            pipe_args.rerank_score_threshold = args.rerank_score_thres;
            pipe_args.rag_post_extending     = args.rag_post_extending;
            chatllm::RAGPipeline pipeline(args.model_path, pipe_args,
                args.vc, args.vector_store,
                args.embedding_model_path, args.reranker_model_path);
            pipeline.hide_reference = args.hide_reference;
            pipeline.retrieve_top_n = args.retrieve_top_n;
            pipeline.rerank_top_n   = args.rerank_top_n;
            if (args.rag_context_sep.length() > 0)
                pipeline.composer.set_context_sep(args.rag_context_sep);
            if (args.rag_template.length() > 0)
                pipeline.composer.set_prompt_template(args.rag_template);
            chat(args, pipeline);
        }
    }
    catch (std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        exit(EXIT_FAILURE);
    }

    return 0;
}
