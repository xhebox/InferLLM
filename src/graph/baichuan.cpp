#include "baichuan.h"

using namespace inferllm;

void BaiChuanGraph::set_weights_alias() {
    m_weights_name_aliases.clear();
    // clang-format off
    m_weights_name_aliases = {
            {"model.embed_tokens.weight", "tok_embeddings.weight"},
            {"model.layers.x.input_layernorm.weight", "layers.x.attention.norm.weight"},
            {"model.layers.x.self_attn.q_proj.weight", "layers.x.attention.wq.weight"},
            {"model.layers.x.self_attn.k_proj.weight", "layers.x.attention.wk.weight"},
            {"model.layers.x.self_attn.v_proj.weight", "layers.x.attention.wv.weight"},
            {"model.layers.x.self_attn.o_proj.weight", "layers.x.attention.wo.weight"},
            {"model.layers.x.post_attention_layernorm.weight", "layers.x.ffn.norm.weight"},
            {"model.layers.x.mlp.up_proj.weight", "layers.x.ffn.w3.weight"},
            {"model.layers.x.mlp.down_proj.weight", "layers.x.ffn.w2.weight"},
            {"model.layers.x.mlp.gate_proj.weight", "layers.x.ffn.w1.weight"},
            {"model.norm.weight", "head.norm.weight"},
            {"lm_head.weight", "head.output.weight"},
    };
    // clang-format on
}

void BaiChuanGraph::load(
        std::shared_ptr<InputFile> fin, LlmParams& param,
        std::shared_ptr<Vocab> vocab) {
     // verify the magic number wrote when model convert
    uint32_t magic;
    uint32_t version = 0;
    fin->read_raw((char*)&magic, sizeof(magic));
    INFER_ASSERT(magic == 0x123456, "model magic is not create!!!!");

    Header header;
    // load model header
    fin->read_raw((char*)&header.param_offset, sizeof(header.param_offset));
    fin->read_raw((char*)&header.param_length, sizeof(header.param_length));
    fin->read_raw((char*)&header.vocab_offset, sizeof(header.vocab_offset));
    fin->read_raw((char*)&header.vocab_length, sizeof(header.vocab_length));
    fin->read_raw((char*)&header.tensor_offset, sizeof(header.tensor_offset));

    fin->seek(header.param_offset);
    // load param
    fin->read_raw((char*)&param.n_embd, sizeof(param.n_embd));
    fin->read_raw((char*)&param.n_head, sizeof(param.n_head));
    fin->read_raw((char*)&param.n_layer, sizeof(param.n_layer));
    fin->read_raw((char*)&param.n_mult, sizeof(param.n_mult));
    fin->read_raw((char*)&param.n_vocab, sizeof(param.n_vocab));
    m_param = param;

    // load vocab
    fin->seek(header.vocab_offset);
    vocab->load_vocab(fin, param.n_vocab);

    INFER_LOG("total vocab length = %d\n", param.n_vocab);

    // create the graph
    constuct_llm();
    collect_weights();

    fin->seek(header.tensor_offset);
    set_weights_alias();
    size_t weight_length = 0;
    while (true) {
        int32_t n_dims;
        int32_t length;
        int32_t ftype;
        if (fin->eof()) {
            break;
        }

        fin->read_raw(reinterpret_cast<char*>(&n_dims), sizeof(n_dims));
        fin->read_raw(reinterpret_cast<char*>(&length), sizeof(length));
        fin->read_raw(reinterpret_cast<char*>(&ftype), sizeof(ftype));

        if (fin->eof()) {
            break;
        }

        size_t nr_number = 1;
        int32_t shape[2] = {1, 1};
        for (int i = 0; i < n_dims; ++i) {
            fin->read_raw(reinterpret_cast<char*>(&shape[i]), sizeof(shape[i]));
            nr_number *= shape[i];
        }

        std::string name(length, 0);
        fin->read_raw(&name[0], length);
        auto alias_name = get_weight_alias(name);
        INFER_ASSERT(
                m_weights_map.count(alias_name) == 1,
                "Error weight is not found when loading.");
        auto weight = m_weights_map[alias_name];
        if(weight->length() != nr_number) {
            printf("weight %s length is %lu, but model is %lu\n", alias_name.c_str(),
                   weight->length(), nr_number);
        }
        INFER_ASSERT(
                weight->length() == nr_number, "Error length of weight is mismatch.");
        weight->set_file(fin, fin->tell());
        weight->set_dtype(convert_dtype(ftype));
        fin->skip(weight->length_in_byte());
        weight_length += weight->length_in_byte();
    }
    INFER_LOG("total weight length = %lu\n", weight_length);
}

void BaiChuanGraph::constuct_llm() {
    uint32_t embd = m_param.n_embd;
    uint32_t ffn_size = m_param.n_mult;
    uint32_t head = m_param.n_head;
    uint32_t ctx = m_param.n_ctx;
    uint32_t n_vocab = m_param.n_vocab;
    uint32_t rot = embd / head;

    m_input = std::make_shared<Tensor>(device(), name() + ":input");
    std::shared_ptr<Tensor> input = m_input;
    //! embd
    input = add_module<EmbdModule>(
            this, input, embd, n_vocab, model_config(), device(), "");

    int nr_layer = m_param.n_layer;
    for (int i = 0; i < nr_layer; i++) {
        std::string name = "layers." + std::to_string(i);
        //! layer norm
        std::shared_ptr<Tensor> attention_input = input;
        auto norm_out_attention = add_one_opr_module<LayerNorm>(
                                          this, OpIOs{attention_input}, device(),
                                          name + ".attention.norm")
                                          ->add_opr(
                                                  embd, /*mul*/ true, /*bias*/ false,
                                                  /*rms*/ true, /*eps*/ 1e-6);
        //! attentin
        auto attention_output = add_module<AttentionModule<LlamaAttention>>(
                this, norm_out_attention, embd, head, rot, ctx, model_config(),
                device(), name + ".attention", i, false, false, RotMode::ModelRotHalf);
        //! add
        auto add_output = add_one_opr_module<Elemwise>(
                                  this, OpIOs{attention_input, attention_output},
                                  device(), name + ".attention_add")
                                  ->add_opr(ElemMode::Add);

        std::shared_ptr<Tensor> feed_forward_input = add_output;
        //! layer normal
        auto ffn_norm_out =
                add_one_opr_module<LayerNorm>(
                        this, OpIOs{feed_forward_input}, device(), name + ".ffn.norm")
                        ->add_opr(
                                embd, /*mul*/ true, /*bias*/ false,
                                /*rms*/ true, /*eps*/ 1e-6);
        //! feed forward
        auto ffn_output = add_module<LlamaFFNModule>(
                this, ffn_norm_out, embd, ffn_size, model_config(), device(), name);
        //! add
        input = add_one_opr_module<Elemwise>(
                        this, OpIOs{feed_forward_input, ffn_output}, device(),
                        name + ".ffn_add")
                        ->add_opr(ElemMode::Add);
    }
    //! the last layer
    m_output = add_module<HeadModule>(
            this, input, embd, n_vocab, model_config(), device(), "head");
}
