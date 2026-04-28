#include <grpcpp/grpcpp.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "embedding.grpc.pb.h"

/*
 * KVLite embedding gRPC C++ 客户端测试程序。
 *
 * 目标：
 * 1. 验证 C++ gRPC 编译链路；
 * 2. 验证 C++ 客户端能连接 Python embedding server；
 * 3. 验证 Health 接口；
 * 4. 验证 Embed 接口；
 * 5. 为后续封装 C ABI 接入 kvs_vector.c 做准备。
 */

class EmbeddingClient {
public:
    explicit EmbeddingClient(const std::string& addr) : stub_(kvlite::embedding::EmbeddingService::NewStub(
        grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()))) {}

    bool Health(double timeout_seconds){
        grpc::ClientContext context;
        kvlite::embedding::HealthRequest request;
        kvlite::embedding::HealthResponse response;

        SetDeadline(context, timeout_seconds);

        grpc::Status status = stub_->Health(&context, request, &response);
        if (!status.ok()) {
            std::cerr << "Health failed: code=" << status.error_code()
                      << " message=" << status.error_message() << std::endl;
            return false;
        }

        std::cout << "health:" << std::endl;
        std::cout << "  ok=" << (response.ok() ? "true" : "false") << std::endl;
        std::cout << "  provider=" << response.provider() << std::endl;
        std::cout << "  model=" << response.model() << std::endl;
        std::cout << "  dimension=" << response.dimension() << std::endl;

        if (!response.ok()) {
            std::cerr << "Health returned ok=false" << std::endl;
            return false;
        }

        if (response.dimension() <= 0) {
            std::cerr << "Health returned invalid dimension" << std::endl;
            return false;
        }

        return true;
    }

     bool Embed(const std::vector<std::string> &texts, double timeout_seconds){
        grpc::ClientContext context;
        kvlite::embedding::EmbedRequest request;
        kvlite::embedding::EmbedResponse response;

        SetDeadline(context, timeout_seconds);

        request.set_normalize(true);

        for (const std::string& text : texts) {
            request.add_texts(text);
        }

        grpc::Status status = stub_->Embed(&context, request, &response);
        if (!status.ok()) {
            std::cerr << "Embed failed: code=" << status.error_code()
                      << " message=" << status.error_message() << std::endl;
            return false;
        }

        std::cout << "embed:" << std::endl;
        std::cout << "  model=" << response.model() << std::endl;
        std::cout << "  dimension=" << response.dimension() << std::endl;
        std::cout << "  count=" << response.embeddings_size() << std::endl;

        if (response.embeddings_size() != static_cast<int>(texts.size())) {
            std::cerr << "embedding count mismatch" << std::endl;
            return false;
        }

        if (response.dimension() <= 0) {
            std::cerr << "invalid response dimension" << std::endl;
            return false;
        }

        for (int i = 0; i < response.embeddings_size(); ++i) {
            const kvlite::embedding::Embedding &embedding = response.embeddings(i);

            std::cout << "  [" << i << "] text=\"" << texts[static_cast<size_t>(i)]
                      << "\" dimension=" << embedding.dimension()
                      << " first5=";

            int preview = embedding.values_size() < 5 ? embedding.values_size() : 5;
            std::cout << "[";
            for (int j = 0; j < preview; ++j) {
                if (j > 0) {
                    std::cout << ", ";
                }
                std::cout << embedding.values(j);
            }
            std::cout << "]" << std::endl;

            if (embedding.dimension() <= 0) {
                std::cerr << "invalid embedding dimension at index " << i << std::endl;
                return false;
            }

            if (embedding.values_size() != embedding.dimension()) {
                std::cerr << "values size mismatch at index " << i
                          << ", values_size=" << embedding.values_size()
                          << ", dimension=" << embedding.dimension() << std::endl;
                return false;
            }

            if (embedding.dimension() != response.dimension()) {
                std::cerr << "embedding dimension differs from response dimension at index "
                          << i << std::endl;
                return false;
            }
        }

        return true;
    }
private:
    static void SetDeadline(grpc::ClientContext &context, double timeout_seconds) {
        if (timeout_seconds <= 0.0) {
            timeout_seconds = 3.0;
        }

        int timeout_ms = static_cast<int>(timeout_seconds * 1000.0);
        context.set_deadline(
            std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms));
    }
    std::unique_ptr<kvlite::embedding::EmbeddingService::Stub> stub_;
};

int main(int argc, char **argv) {
    std::string addr = "127.0.0.1:50051";
    double timeout_seconds = 3.0;

    if (argc >= 2) {
        addr = argv[1];
    }

    if (argc >= 3) {
        timeout_seconds = std::atof(argv[2]);
    }

    std::cout << "connecting embedding server: " << addr << std::endl;

    EmbeddingClient client(addr);

    if (!client.Health(timeout_seconds)) {
        return 1;
    }

    std::vector<std::string> texts;
    texts.emplace_back("你好 世界");
    texts.emplace_back("hello world");
    texts.emplace_back("向量检索测试");

    if (!client.Embed(texts, timeout_seconds)) {
        return 1;
    }

    std::cout << "C++ embedding gRPC client test passed" << std::endl;
    return 0;
}