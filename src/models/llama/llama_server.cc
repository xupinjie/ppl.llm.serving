// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "utils/index_manager.h"
#include "utils/queue_request_scheduler.h"
#include "llama_worker.h"
#include "resource.h"
#include "serving/grpc_server.h"
#include "sampler/cuda/sampler.h"

#include "ppl/common/log.h"
#include "ppl/nn/engines/llm_cuda/engine_factory.h"
#include "ppl/nn/runtime/tensor.h"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"

#include <nccl.h>
#include <omp.h>
#include <fstream>

using namespace std;
using namespace ppl::llm;
using namespace ppl::llm::llama;
using namespace ppl::common;
using namespace ppl::nn;

struct ServerConfig {
    std::string model_dir;
    std::string model_param_path;
    std::string tokenizer_path;

    int tensor_parallel_size;

    float top_p;
    int top_k;

    float max_tokens_scale;
    int max_tokens_per_request;
    int max_running_batch;

    std::string host;
    int port;
};

bool ParseServerConfig(const std::string& config_file, ServerConfig* server_config) {
    std::ifstream ifs(config_file);
    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document json_reader;
    json_reader.ParseStream(isw);
    if (json_reader.HasParseError()) {
        LOG(ERROR) << "Parse Json Error, server config file: " << config_file;
        return false;
    }

    auto it = json_reader.FindMember("model_dir");
    if (it == json_reader.MemberEnd()) {
        LOG(ERROR) << "find key [model_dir] failed";
        return false;
    }
    server_config->model_dir = it->value.GetString();

    it = json_reader.FindMember("model_param_path");
    if (it == json_reader.MemberEnd()) {
        LOG(ERROR) << "find key [model_param_path] failed";
        return false;
    }
    server_config->model_param_path = it->value.GetString();

    it = json_reader.FindMember("tokenizer_path");
    if (it == json_reader.MemberEnd()) {
        LOG(ERROR) << "find key [tokenizer_path] failed";
        return false;
    }
    server_config->tokenizer_path = it->value.GetString();

    it = json_reader.FindMember("tensor_parallel_size");
    if (it == json_reader.MemberEnd()) {
        LOG(ERROR) << "find key [tensor_parallel_size] failed";
        return false;
    }
    server_config->tensor_parallel_size = it->value.GetInt();

    it = json_reader.FindMember("top_p");
    if (it == json_reader.MemberEnd()) {
        LOG(ERROR) << "find key [top_p] failed";
        return false;
    }
    server_config->top_p = std::min(std::max(it->value.GetFloat(), 0.0f), 1.0f);

    it = json_reader.FindMember("top_k");
    if (it == json_reader.MemberEnd()) {
        LOG(ERROR) << "find key [top_k] failed";
        return false;
    }
    server_config->top_k = std::max(it->value.GetInt(), 1);

    it = json_reader.FindMember("max_tokens_scale");
    if (it == json_reader.MemberEnd()) {
        LOG(ERROR) << "find key [max_tokens_scale] failed";
        return false;
    }
    server_config->max_tokens_scale = std::max(it->value.GetFloat(), 0.1f);

    it = json_reader.FindMember("max_tokens_per_request");
    if (it == json_reader.MemberEnd()) {
        LOG(ERROR) << "find key [max_tokens_per_request] failed";
        return false;
    }
    server_config->max_tokens_per_request = std::max(it->value.GetInt(), 1);

    it = json_reader.FindMember("max_running_batch");
    if (it == json_reader.MemberEnd()) {
        LOG(ERROR) << "find key [max_running_batch] failed";
        return false;
    }
    server_config->max_running_batch = std::max(it->value.GetInt(), 1);

    it = json_reader.FindMember("host");
    if (it == json_reader.MemberEnd()) {
        LOG(ERROR) << "find key [host] failed";
        return false;
    }
    server_config->host = it->value.GetString();

    it = json_reader.FindMember("port");
    if (it == json_reader.MemberEnd()) {
        LOG(ERROR) << "find key [port] failed";
        return false;
    }
    server_config->port = it->value.GetInt();

    LOG(INFO) << "server_config.host: " << server_config->host;
    LOG(INFO) << "server_config.port: " << server_config->port;

    LOG(INFO) << "server_config.model_dir: " << server_config->model_dir;
    LOG(INFO) << "server_config.model_param_path: " << server_config->model_param_path;
    LOG(INFO) << "server_config.tokenizer_path: " << server_config->tokenizer_path;

    LOG(INFO) << "server_config.top_k: " << server_config->top_k;
    LOG(INFO) << "server_config.top_p: " << server_config->top_p;

    LOG(INFO) << "server_config.tensor_parallel_size: " << server_config->tensor_parallel_size;
    LOG(INFO) << "server_config.max_tokens_scale: " << server_config->max_tokens_scale;
    LOG(INFO) << "server_config.max_tokens_per_request: " << server_config->max_tokens_per_request;
    LOG(INFO) << "server_config.max_running_batch: " << server_config->max_running_batch;

    return true;
}

bool InitJsonReader(const std::string config_file, rapidjson::Document* document) {
    std::ifstream ifs(config_file);
    rapidjson::IStreamWrapper isw(ifs);
    document->ParseStream(isw);
    if (document->HasParseError()) {
        LOG(ERROR) << "Parse Json Error, model_config file: " << config_file;
        return false;
    }
    return true;
}

bool ParseModelConfig(const std::string& model_param_path, ModelConfig* model_config) {
    std::ifstream ifs(model_param_path);
    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document document;
    if (document.ParseStream(isw) == false) {
        LOG(ERROR) << "ParseStream failed";
        return false;
    }
    document.ParseStream(isw);

    auto it = document.FindMember("num_heads");
    if (it == document.MemberEnd()) {
        LOG(ERROR) << "find key [num_heads] failed";
        return false;
    }
    model_config->num_heads = it->value.GetInt();

    it = document.FindMember("num_kv_heads");
    if (it == document.MemberEnd()) {
        model_config->num_kv_heads = model_config->num_heads;
    } else {
        model_config->num_kv_heads = it->value.GetInt();
    }

    it = document.FindMember("num_layers");
    if (it == document.MemberEnd()) {
        LOG(ERROR) << "find key [num_layers] failed";
        return false;
    }
    model_config->num_layers = it->value.GetInt();

    it = document.FindMember("hidden_dim");
    if (it == document.MemberEnd()) {
        LOG(ERROR) << "find key [hidden_dim] failed";
        return false;
    }
    model_config->hidden_dim = it->value.GetInt();

    it = document.FindMember("intermediate_dim");
    if (it == document.MemberEnd()) {
        LOG(ERROR) << "find key [intermediate_dim] failed";
        return false;
    }
    model_config->intermediate_dim = it->value.GetInt();

    it = document.FindMember("vocab_size");
    if (it == document.MemberEnd()) {
        LOG(ERROR) << "find key [vocab_size] failed";
        return false;
    }
    model_config->vocab_size = it->value.GetInt();

    it = document.FindMember("cache_quant_bit");
    if (it == document.MemberEnd()) {
        LOG(ERROR) << "find key [cache_quant_bit] failed";
        return false;
    }
    model_config->cache_quant_bit = it->value.GetInt();

    it = document.FindMember("cache_quant_group");
    if (it == document.MemberEnd()) {
        LOG(ERROR) << "find key [cache_quant_group] failed";
        return false;
    }
    model_config->cache_quant_group = it->value.GetInt();

    it = document.FindMember("cache_layout");
    if (it == document.MemberEnd()) {
        LOG(ERROR) << "find key [cache_layout] failed";
        return false;
    }
    model_config->cache_layout = it->value.GetInt();

    it = document.FindMember("cache_mode");
    if (it == document.MemberEnd()) {
        LOG(ERROR) << "find key [cache_mode] failed";
        return false;
    }
    model_config->cache_mode = it->value.GetInt();

    it = document.FindMember("dynamic_batching");
    if (it == document.MemberEnd()) {
        LOG(ERROR) << "find key [dynamic_batching] failed";
        return false;
    }
    model_config->dynamic_batching = it->value.GetBool();

    it = document.FindMember("auto_causal");
    if (it == document.MemberEnd()) {
        LOG(ERROR) << "find key [auto_causal] failed";
        return false;
    }
    model_config->auto_causal = it->value.GetBool();

    LOG(INFO) << "model_config.num_layers: " << model_config->num_layers;
    LOG(INFO) << "model_config.num_heads: " << model_config->num_heads;
    LOG(INFO) << "model_config.num_kv_heads: " << model_config->num_kv_heads;
    LOG(INFO) << "model_config.hidden_dim: " << model_config->hidden_dim;
    LOG(INFO) << "model_config.intermediate_dim: " << model_config->intermediate_dim;
    LOG(INFO) << "model_config.vocab_size: " << model_config->vocab_size;

    LOG(INFO) << "model_config.cache_quant_bit: " << model_config->cache_quant_bit;
    LOG(INFO) << "model_config.cache_quant_group: " << model_config->cache_quant_group;
    LOG(INFO) << "model_config.cache_layout: " << model_config->cache_layout;
    LOG(INFO) << "model_config.cache_mode: " << model_config->cache_mode;

    LOG(INFO) << "model_config.dynamic_batching: " << model_config->dynamic_batching;
    LOG(INFO) << "model_config.auto_causal: " << model_config->auto_causal;

    return true;
}

#define NCCL_CHECK(cmd, emsg)                                                \
    do {                                                                     \
        ncclResult_t e = (cmd);                                              \
        if (e != ncclSuccess) {                                              \
            LOG(ERROR) << "NCCL error(code:" << (int)e << ") on " << (emsg); \
            return RC_OTHER_ERROR;                                           \
        }                                                                    \
    } while (0);

static RetCode InitNccl(uint32_t tensor_parallel_size, vector<ncclComm_t>* nccl_comm_list) {
    nccl_comm_list->resize(tensor_parallel_size);
    std::vector<int> dev_list(tensor_parallel_size);
    for (size_t i = 0; i < tensor_parallel_size; ++i) {
        dev_list[i] = i;
    }
    NCCL_CHECK(ncclCommInitAll(nccl_comm_list->data(), tensor_parallel_size, dev_list.data()), "ncclCommInitAll");
    return RC_SUCCESS;
}

static Engine* CreateCudaEngine(ncclComm_t nccl_comm, int device_id) {
    ppl::nn::llm::cuda::EngineOptions options;
    options.device_id = device_id;
    options.mm_policy = ppl::nn::llm::cuda::MM_COMPACT;

    auto engine = unique_ptr<Engine>(ppl::nn::llm::cuda::EngineFactory::Create(options));
    if (!engine) {
        LOG(ERROR) << "create cuda engine failed.";
        return nullptr;
    }

    auto rc = engine->Configure(ppl::nn::llm::cuda::ENGINE_CONF_SET_TP_NCCL_COMM, nccl_comm);
    if (rc != RC_SUCCESS) {
        return nullptr;
    }

    return engine.release();
}

static shared_ptr<Sampler> CreateCudaSampler(Runtime* runtime) {
    ppl::nn::DeviceContext::Type needed_type;
    *((int64_t*)needed_type.str) = 0;
    needed_type.str[0] = 'c';
    needed_type.str[1] = 'u';
    needed_type.str[2] = 'd';
    needed_type.str[3] = 'a';

    ppl::nn::DeviceContext* dev = nullptr;
    for (uint32_t i = 0; i < runtime->GetDeviceContextCount(); ++i) {
        if (runtime->GetDeviceContext(i)->GetType() == needed_type) {
            dev = runtime->GetDeviceContext(i);
            break;
        }
    }

    if (!dev) {
        LOG(ERROR) << "cannot find cuda device in runtime.";
        return nullptr;
    }

    auto cu_sampler = make_shared<cuda::Sampler>(dev);
    auto rc = cu_sampler->Init();
    if (RC_SUCCESS != rc) {
        LOG(ERROR) << "cu_sampler->Init() failed: " << GetRetCodeStr(rc);
        return shared_ptr<Sampler>();
    }

    return cu_sampler;
}

static Runtime* CreatePPLRuntime(Engine* cuda_engine, const string& model_file) {
    auto builder = unique_ptr<onnx::RuntimeBuilder>(onnx::RuntimeBuilderFactory::Create());
    if (!builder) {
        LOG(ERROR) << "create onnx builder failed.";
        return nullptr;
    }

    auto rc = builder->LoadModel(model_file.c_str());
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "load model [" << model_file << "] failed: " << GetRetCodeStr(rc);
        return nullptr;
    }

    onnx::RuntimeBuilder::Resources resources;
    resources.engines = &cuda_engine;
    resources.engine_num = 1;

    rc = builder->SetResources(resources);
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "set resources for builder failed: " << GetRetCodeStr(rc);
        return nullptr;
    }

    rc = builder->Preprocess();
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "builder preprocess failed: " << GetRetCodeStr(rc);
        return nullptr;
    }

    return builder->CreateRuntime();
}

struct ResourceManager final {
    ~ResourceManager() {
        for (auto it = items.begin(); it != items.end(); ++it) {
            cudaFree(it->kv_cache_mem);
            cudaFree(it->kv_scale_mem);
            delete it->runtime;
        }
        items.clear();

        engine_list.clear();

        for (auto it = nccl_comm_list.begin(); it != nccl_comm_list.end(); ++it) {
            ncclCommDestroy(*it);
        }
        nccl_comm_list.clear();
    }

    vector<ResourceItem> items;
    vector<unique_ptr<Engine>> engine_list;
    vector<ncclComm_t> nccl_comm_list;
    uint64_t kv_cache_max_tokens;
};

static RetCode InitResourceManager(uint32_t tensor_parallel_size, uint64_t kv_cache_block_bytes,
                                   uint64_t kv_scale_block_bytes, float kv_cache_max_tokens_scale,
                                   const string& model_dir, ResourceManager* mgr) {
    auto rc = InitNccl(tensor_parallel_size, &mgr->nccl_comm_list);
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "NCCL init failed.";
        exit(-1);
    }
    LOG(INFO) << "Init Nccl successed";

    mgr->items.resize(tensor_parallel_size);
    mgr->engine_list.resize(tensor_parallel_size);

    #pragma omp parallel num_threads(tensor_parallel_size)
    {
        int thread_id = omp_get_thread_num();

        auto engine = unique_ptr<Engine>(CreateCudaEngine(mgr->nccl_comm_list[thread_id], thread_id));
        if (!engine) {
            LOG(ERROR) << "create cuda engine failed.";
            exit(-1);
        }
        LOG(INFO) << "create engine success";

        unique_ptr<Runtime> runtime;
        // load models one by one to reduce memory usage
        #pragma omp critical
        {
            const std::string model_path = model_dir + "/model_slice_" + std::to_string(thread_id) + "/model.onnx";
            LOG(INFO) << "model_slice_" << std::to_string(thread_id) << ": " << model_path;
            runtime = unique_ptr<Runtime>(CreatePPLRuntime(engine.get(), model_path));
            if (!runtime) {
                LOG(ERROR) << "create runtime failed.";
                exit(-1);
            }
        }
        mgr->engine_list[thread_id] = std::move(engine);

        #pragma omp barrier // wait untill model loadeds

        #pragma omp master
        {
            size_t avail_bytes = -1, total = -1;
            cudaMemGetInfo(&avail_bytes, &total);
            uint64_t kv_cache_max_bytes = kv_cache_max_tokens_scale * avail_bytes * (kv_cache_block_bytes) /
                (kv_cache_block_bytes + kv_scale_block_bytes);
            uint64_t kv_scale_max_bytes = kv_cache_max_tokens_scale * avail_bytes * (kv_scale_block_bytes) /
                (kv_cache_block_bytes + kv_scale_block_bytes);
            LOG(INFO) << "avail_bytes: " << avail_bytes;
            LOG(INFO) << "kv_cache_max_bytes: " << kv_cache_max_bytes;
            LOG(INFO) << "kv_scale_max_bytes: " << kv_scale_max_bytes;

            mgr->kv_cache_max_tokens = kv_cache_max_bytes / kv_cache_block_bytes;
            LOG(INFO) << "max_tokens: " << mgr->kv_cache_max_tokens;
        }

        #pragma omp barrier

        ResourceItem item;

        auto cu_ret = cudaMalloc(&item.kv_cache_mem, mgr->kv_cache_max_tokens * kv_cache_block_bytes);
        if (cu_ret != cudaSuccess) {
            LOG(ERROR) << "alloc kv cache [" << mgr->kv_cache_max_tokens * kv_cache_block_bytes << "] failed.";
            exit(-1);
        }
        cu_ret = cudaMalloc(&item.kv_scale_mem, mgr->kv_cache_max_tokens * kv_scale_block_bytes);
        if (cu_ret != cudaSuccess) {
            cudaFree(item.kv_cache_mem);
            LOG(ERROR) << "alloc kv scale [" << mgr->kv_cache_max_tokens * kv_scale_block_bytes << "] failed.";
            exit(-1);
        }
        item.runtime = runtime.release();

        mgr->items[thread_id] = item;
    }

    return rc;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "usage: " << argv[0] << " server_config.json" << endl;
        return -1;
    }

    ServerConfig server_config;
    if (!ParseServerConfig(argv[1], &server_config)) {
        LOG(ERROR) << "ParseServerConfig failed, server config file: " << argv[1];
        return -1;
    }

    WorkerConfig worker_config;
    worker_config.max_tokens_per_request = server_config.max_tokens_per_request;
    worker_config.max_running_batch = server_config.max_running_batch;
    worker_config.top_p = server_config.top_p;
    worker_config.top_k = server_config.top_k;

    ModelConfig model_config;
    if (!ParseModelConfig(server_config.model_param_path, &model_config)) {
        LOG(ERROR) << "PaseModelConfig failed, model_param_path: " << server_config.model_param_path;
        return -1;
    }
    LOG(INFO) << "Parse model model_config successed";

    const uint64_t kv_cache_block_bytes = model_config.num_layers * 2 * model_config.num_kv_heads /
        server_config.tensor_parallel_size * model_config.hidden_dim / model_config.num_heads * sizeof(int8_t);
    const uint64_t kv_scale_block_bytes = model_config.num_layers * 2 * model_config.num_kv_heads /
        server_config.tensor_parallel_size * model_config.hidden_dim / model_config.num_heads /
        model_config.cache_quant_group * sizeof(float16_t);

    // init nccl, cuda engine, kv cache, kv scale manager
    ResourceManager resource_manager;
    auto rc = InitResourceManager(server_config.tensor_parallel_size, kv_cache_block_bytes, kv_scale_block_bytes,
                                  server_config.max_tokens_scale, server_config.model_dir, &resource_manager);

    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "init ResourceManager failed: " << GetRetCodeStr(rc);
        return -1;
    }

    sentencepiece::SentencePieceProcessor tokenizer;
    auto tokenizer_status = tokenizer.Load(server_config.tokenizer_path);
    if (!tokenizer_status.ok()) {
        LOG(ERROR) << tokenizer_status.ToString();
        return -1;
    }
    LOG(INFO) << "VOCAB_SIZE: " << tokenizer.GetPieceSize() << "; BOS ID: " << tokenizer.bos_id()
              << "; EOS ID: " << tokenizer.eos_id() << "; PAD ID: " << tokenizer.pad_id();

    // make sure that Sampler is created before LLaMAWorker
    auto sampler = CreateCudaSampler(resource_manager.items[0].runtime);
    if (!sampler) {
        LOG(ERROR) << "CreateCudaSampler failed";
        return -1;
    }

    Resource resource;
    resource.tensor_parallel_size = server_config.tensor_parallel_size;
    resource.kv_cache_max_tokens = resource_manager.kv_cache_max_tokens;
    resource.items = resource_manager.items.data();
    LLaMAWorker llama_worker(&tokenizer, resource, model_config, worker_config);

    rc = llama_worker.Init();
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "llama_worker init failed: " << GetRetCodeStr(rc);
        return -1;
    }
    LOG(INFO) << "Init llama worker successed";

    llama_worker.SetSampler(sampler);

    auto conn = make_shared<GRPCServer>();
    auto listen_addr = server_config.host + ":" + std::to_string(server_config.port);
    rc = conn->Init(listen_addr);
    if (rc != RC_SUCCESS) {
        LOG(ERROR) << "GRPCConnection init failed.";
        return -1;
    }

    conn->SetOnDisconnectedFunc([&llama_worker](Connection* c) {
        llama_worker.ClearTask(c);
    });
    LOG(INFO) << "listening on [" << listen_addr << "]";

    conn->Loop(&llama_worker);

    return 0;
}