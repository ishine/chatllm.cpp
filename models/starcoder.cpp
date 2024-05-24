namespace v2
{
    struct Config : public BaseConfig
    {
        int num_key_value_heads;
        int sliding_window;
        float rope_theta;
    };

    const int SLIDING_WINDOW_LEN            =  4096;

    class Tokenizer : public BaseTokenizer
    {
    public:
        Tokenizer(const Config &config)
            : Tokenizer(config, nullptr)
        {}

        Tokenizer(const Config &config, BaseHistoryEncoder *encoder)
            : BaseTokenizer::BaseTokenizer(config, encoder)
        {
            sys_prompt = "";
        }

        size_t load(const char *buffer, int n_vocab) override;
    };

    class ConditionalGeneration : public BaseModelForConditionalGeneration<
                                    Model<Config, Embedding, LayerNorm, StarCoder2Block<SLIDING_WINDOW_LEN>, int, int, int, int, int>>
    {
    public:
        ConditionalGeneration(const Config &config);

        void load(ModelLoader &loader) override;

    public:
        static constexpr size_t MEM_SIZE = 1812ull * 1024 * 1024;
        static constexpr size_t SCRATCH_SIZE = 444ull * 1024 * 1024;

        Config config;

    private:
        // hold ggml_context & kv_cache
        InitContext w_ctx_; // weight context
    };

    size_t Tokenizer::load(const char *buffer, int n_vocab)
    {
        tp = new tokenizer::BPEProcessor2();
        size_t size = tp->Load(buffer, n_vocab);
        return size;
    }

    ConditionalGeneration::ConditionalGeneration(const Config &config)
        : BaseModelForConditionalGeneration<
                                    Model<Config, Embedding, LayerNorm, StarCoder2Block<SLIDING_WINDOW_LEN>, int, int, int, int, int>>(MODEL_TYPE_STARCODER2, config, MEM_SIZE, SCRATCH_SIZE), config(config)
    {
        constexpr size_t tensor_ovhd = GGML_TENSOR_SIZE + GGML_OBJECT_SIZE;
        const size_t num_tensors = 3 + config.num_hidden_layers * 20;
        const size_t ctx_size = num_tensors * tensor_ovhd;
        w_ctx_.gctx = GGMLContext({.mem_size = ctx_size, .mem_buffer = nullptr, .no_alloc = true});
        w_ctx_.dtype = config.dtype;

        CHATLLM_CHECK(config.sliding_window == SLIDING_WINDOW_LEN)
            << "sliding_window (" << config.sliding_window << ") must be " << SLIDING_WINDOW_LEN;

        transformer = new Model<Config, Embedding, LayerNorm, StarCoder2Block<SLIDING_WINDOW_LEN>, int, int, int, int, int>(&w_ctx_, config, nullptr,
                                                            config.hidden_size, config.num_attention_heads,
                                                            config.intermediate_size, config.num_key_value_heads, config.max_length);

        for (int i = 0; i < config.num_hidden_layers; i++)
        {
            auto &layer = transformer->layers[i];
            layer.attention.freq_base = config.rope_theta;
        }

        batch_input = false;
    }

    void ConditionalGeneration::load(ModelLoader &loader)
    {
        loader.read_tensor("model.embed_tokens.weight", transformer->word_embeddings.weight);
        for (int i = 0; i < config.num_hidden_layers; i++)
        {
            std::string layer_prefix = "model.layers." + std::to_string(layer_ids[i]) + '.';

            loader.read_tensor(layer_prefix + "input_layernorm.weight", transformer->layers[i].input_layernorm.weight);
            loader.read_tensor(layer_prefix + "input_layernorm.bias",   transformer->layers[i].input_layernorm.bias);

            loader.read_tensor(layer_prefix + "mlp.c_fc.weight",    transformer->layers[i].mlp.fc0.weight);
            loader.read_tensor(layer_prefix + "mlp.c_fc.bias",      transformer->layers[i].mlp.fc0.bias);
            loader.read_tensor(layer_prefix + "mlp.c_proj.weight",  transformer->layers[i].mlp.fc1.weight);
            loader.read_tensor(layer_prefix + "mlp.c_proj.bias",    transformer->layers[i].mlp.fc1.bias);

            loader.read_tensor(layer_prefix + "post_attention_layernorm.weight", transformer->layers[i].post_attention_layernorm.weight);
            loader.read_tensor(layer_prefix + "post_attention_layernorm.bias",   transformer->layers[i].post_attention_layernorm.bias);

            loader.read_tensor(layer_prefix + "self_attn.k_proj.weight", transformer->layers[i].attention.k_proj.weight);
            loader.read_tensor(layer_prefix + "self_attn.k_proj.bias",   transformer->layers[i].attention.k_proj.bias);
            loader.read_tensor(layer_prefix + "self_attn.o_proj.weight", transformer->layers[i].attention.o_proj.weight);
            loader.read_tensor(layer_prefix + "self_attn.o_proj.bias",   transformer->layers[i].attention.o_proj.bias);
            loader.read_tensor(layer_prefix + "self_attn.q_proj.weight", transformer->layers[i].attention.q_proj.weight);
            loader.read_tensor(layer_prefix + "self_attn.q_proj.bias",   transformer->layers[i].attention.q_proj.bias);
            loader.read_tensor(layer_prefix + "self_attn.v_proj.weight", transformer->layers[i].attention.v_proj.weight);
            loader.read_tensor(layer_prefix + "self_attn.v_proj.bias",   transformer->layers[i].attention.v_proj.bias);
        }
        loader.read_tensor("model.norm.weight", transformer->final_layernorm.weight);
        loader.read_tensor("model.norm.bias",   transformer->final_layernorm.bias);

        CHATLLM_CHECK(ggml_used_mem(w_ctx_.gctx.get()) == ggml_get_mem_size(w_ctx_.gctx.get()))
            << "corrupted model weights";
    }
}
