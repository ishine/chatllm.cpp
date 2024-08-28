struct Config : public llama::v2::Config
{
    int num_key_value_heads;
    float rope_scaling;
    float rope_theta;
};

class ChatHistoryEncoder : public BaseHistoryEncoder
{
public:
    void append_sys_prompt(std::vector<int> &ids) const override;
    void append_ai(int round_idx, const std::string &ai, std::vector<int> &ids) const override;
    void append_user(int round_idx, const std::string &user, std::vector<int> &ids) const override;
    void append_ai_opening(int round_idx, std::vector<int> &ids) const override;
};

static ChatHistoryEncoder _chat_encoder;

class Tokenizer : public llama::v2::Tokenizer
{
public:
    Tokenizer(const Config &config)
        : llama::v2::Tokenizer::Tokenizer(config, &_chat_encoder)
    {
        sys_prompt = "";
    }

    size_t load(tokenizer::DataReader *buffer, int n_vocab) override;

    bool is_special_id(int id) const override;

public:
    int im_start_token_id;
    int im_end_token_id;
    int im_sep_token_id;
};

class ConditionalGeneration : public BaseModelForConditionalGeneration
{
public:
    typedef Model<Config, Embedding, RMSNorm, LlamaBlock, int, int, int, int, int> ModelClass;
public:
    ConditionalGeneration() = default;
    ConditionalGeneration(const Config &config, const RuntimeConfig &runtime_config, ModelType type = ModelType::MODEL_TYPE_YI);

    void load(ModelLoader &loader) override;

public:
    Config config;
};

size_t Tokenizer::load(tokenizer::DataReader *buffer, int n_vocab)
{
    tp = new tokenizer::BPEProcessor1();
    size_t size = tp->Load(buffer, n_vocab);
    bos_token_id = tp->GetPieceSize();
    eos_token_id = bos_token_id + 1;
    im_start_token_id = 6;
    im_end_token_id = 7;
    im_sep_token_id = 8;
    terminate_ids.insert(im_end_token_id);
    return size;
}

void ChatHistoryEncoder::append_ai(int round_idx, const std::string &ai, std::vector<int> &ids) const
{
    Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);
    append_ai_opening(round_idx, ids);

    tok->encode(ai, ids);
    ids.push_back(tok->im_end_token_id);
    tok->encode("\n", ids);
}

void ChatHistoryEncoder::append_sys_prompt(std::vector<int> &ids) const
{
    Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);
    if (tok->get_system_prompt().size() > 0)
    {
        ids.push_back(tok->im_start_token_id);
        tok->encode("system" + tok->get_system_prompt(), ids);
        ids.push_back(tok->im_end_token_id);
        tok->encode("\n", ids);
    }
}

void ChatHistoryEncoder::append_user(int round_idx, const std::string &user, std::vector<int> &ids) const
{
    Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);

    ids.push_back(tok->im_start_token_id);
    tok->encode("user\n" + user, ids);
    ids.push_back(tok->im_end_token_id);
    tok->encode("\n", ids);
}

void ChatHistoryEncoder::append_ai_opening(int round_idx, std::vector<int> &ids) const
{
    Tokenizer *tok = dynamic_cast<Tokenizer *>(tokenizer);

    ids.push_back(tok->im_start_token_id);
    tok->encode("assistant\n", ids);
}

bool Tokenizer::is_special_id(int id) const
{
    return llama::v2::Tokenizer::is_special_id(id)
            || (id == bos_token_id)
            || (id == eos_token_id)
            || (id == im_start_token_id)
            || (id == im_end_token_id)
            || (id == im_sep_token_id);
}

ConditionalGeneration::ConditionalGeneration(const Config &config, const RuntimeConfig &runtime_config, ModelType type)
    : BaseModelForConditionalGeneration(type, config, runtime_config), config(config)
{
    constexpr size_t tensor_ovhd = GGML_TENSOR_SIZE + GGML_OBJECT_SIZE;
    const size_t num_tensors = 3 + config.num_hidden_layers * 12;
    const size_t ctx_size = num_tensors * tensor_ovhd;
    w_ctx_.gctx = GGMLContext({.mem_size = ctx_size, .mem_buffer = nullptr, .no_alloc = true});
    w_ctx_.dtype = config.dtype;

    transformer = new ModelClass(&w_ctx_, config, false,
                                config.hidden_size, config.num_attention_heads,
                                config.intermediate_size, config.num_key_value_heads, config.max_length);

    for (int i = 0; i < config.num_hidden_layers; i++)
    {
        auto &attention = get_typed_transformer<ModelClass>()->layers[i].attention;
        attention.freq_base = config.rope_theta;
        attention.freq_scale = 1 / config.rope_scaling;
    }

    if (transformer->get_param_num(false) > 20000000)
        GRAPH_SIZE = 4096;
}

void ConditionalGeneration::load(ModelLoader &loader)
{
    auto transformer = get_typed_transformer<ModelClass>();
    loader.read_tensor("model.embed_tokens.weight", transformer->word_embeddings.weight);
    for (int i = 0; i < config.num_hidden_layers; i++)
    {
        std::string layer_prefix = "model.layers." + std::to_string(layer_ids[i]) + '.';
        loader.read_tensor(layer_prefix + "input_layernorm.weight",
                           transformer->layers[i].input_layernorm.weight);
        loader.read_tensor(layer_prefix + "mlp.down_proj.weight", transformer->layers[i].mlp.down_proj.weight);
        loader.read_tensor(layer_prefix + "mlp.gate_proj.weight", transformer->layers[i].mlp.gate_proj.weight);
        loader.read_tensor(layer_prefix + "mlp.up_proj.weight", transformer->layers[i].mlp.up_proj.weight);
        loader.read_tensor(layer_prefix + "post_attention_layernorm.weight",
                           transformer->layers[i].post_attention_layernorm.weight);

        loader.read_tensor(layer_prefix + "self_attn.k_proj.weight", transformer->layers[i].attention.k_proj.weight);
        loader.read_tensor(layer_prefix + "self_attn.o_proj.weight", transformer->layers[i].attention.o_proj.weight);
        loader.read_tensor(layer_prefix + "self_attn.q_proj.weight", transformer->layers[i].attention.q_proj.weight);
        loader.read_tensor(layer_prefix + "self_attn.v_proj.weight", transformer->layers[i].attention.v_proj.weight);
    }
    loader.read_tensor("model.norm.weight", transformer->final_layernorm.weight);
    loader.read_tensor("lm_head.weight", dynamic_cast<Linear *>(transformer->lm_head)->weight);

    CHATLLM_CHECK(w_ctx_.get_used_mem() == w_ctx_.get_mem_size())
        << "corrupted model weights";
}