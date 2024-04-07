#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "llm.grpc.pb.h"
#include "sentencepiece_processor.h"

#include <grpc++/grpc++.h>
#include <chrono>

using namespace grpc;
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using namespace std::chrono;
using namespace ppl::llm;

ABSL_FLAG(std::string, target, "localhost:50052", "Server address");

class GenerationClient {
public:
    GenerationClient(std::shared_ptr<Channel> channel) : stub_(proto::LLMService::NewStub(channel)) {}

    int Generation(const std::vector<std::string>& prompts, const sentencepiece::SentencePieceProcessor& tokenizer) {
        // Data we are sending to the server.
        ClientContext context;
        proto::BatchedRequest req_list;
        std::unordered_map<int, std::vector<int>> rsp_stream_store;
        for (size_t i = 0; i < prompts.size(); i++) {
            // request
            auto req = req_list.add_req();
            req->set_id(i);

            std::vector<int> prompt_token_ids;
            tokenizer.Encode(prompts[i], &prompt_token_ids);

            auto* pb_tokens = req->mutable_tokens();
            for (auto token : prompt_token_ids) {
                pb_tokens->add_ids(token);
            }
            req->set_temperature(1);
            auto* stopping_parameters = req->mutable_stopping_parameters();
            stopping_parameters->set_max_new_tokens(64);
            stopping_parameters->set_ignore_eos_token(false);
            rsp_stream_store[i] = {};
        }
        // response
        proto::Response rsp;
        std::unique_ptr<ClientReader<proto::Response> > reader(stub_->Generation(&context, req_list));

        // stream chat
        auto start = system_clock::now();
        auto first_fill_time = system_clock::now();
        bool is_first_fill = true;

        while (reader->Read(&rsp)) {
            if (is_first_fill) {
                first_fill_time = system_clock::now();
                is_first_fill = false;
            }

            int tid = rsp.id();
            // std::string rsp_stream = rsp.generated();
            int token = rsp.tokens().ids().at(0);
            rsp_stream_store[tid].push_back(token);
        }
        auto end = system_clock::now();

        std::cout << "------------------------------" << std::endl;
        std::cout << "--------- Answer -------------" << std::endl;
        std::cout << "------------------------------" << std::endl;

        for (auto rsp : rsp_stream_store) {
            std::string rsp_str;
            tokenizer.Decode(rsp.second.data(), rsp.second.size(), &rsp_str);
            std::cout << rsp_str << std::endl;
            std::cout << "--------------------" << std::endl;
        }

        auto first_till_duration = duration_cast<std::chrono::milliseconds>(first_fill_time - start);
        auto duration = duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "first fill: " << first_till_duration.count() << " ms" << std::endl;

        std::cout << "total: " << duration.count() << " ms" << std::endl;

        Status status = reader->Finish();
        if (status.ok()) {
            std::cout << "Generation rpc succeeded." << std::endl;
        } else {
            std::cerr << "Generation rpc failed." << std::endl;
            return -1;
        }
        return 0;
    }

private:
    std::unique_ptr<proto::LLMService::Stub> stub_;
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " host:port tokenizer_path" << std::endl;
        return -1;
    }

    const std::string target_str = argv[1];
    const std::string tokenizer_path = argv[2];
    sentencepiece::SentencePieceProcessor tokenizer;
    const auto tokenizer_status = tokenizer.Load(tokenizer_path);
    if (!tokenizer_status.ok()) {
        std::cerr << "[ERROR]" << tokenizer_status.ToString() << std::endl;
        return -1;
    }
    std::cout << "VOCAB_SIZE: " << tokenizer.GetPieceSize() << "; BOS ID: " << tokenizer.bos_id()
              << "; EOS ID: " << tokenizer.eos_id() << "; PAD ID: " << tokenizer.pad_id() << std::endl;


    GenerationClient generator(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));

    std::string prompt = "Building a website can be done in 10 simple steps:\n";
    const std::vector<std::string> prompts = {3, prompt};

    std::cout << "------------------------------" << std::endl;
    std::cout << "--------- Question -------------" << std::endl;
    std::cout << "------------------------------" << std::endl;

    for (auto& str : prompts) {
        std::cout << str << std::endl;
    }

    generator.Generation(prompts, tokenizer);
    return 0;
}
