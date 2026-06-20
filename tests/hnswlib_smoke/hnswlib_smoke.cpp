#include <hnswlib/hnswlib.h>
#include <iostream>
#include <vector>

int main() {
    int dim = 4;
    int max_elements = 16;
    int M = 16;
    int ef_construction = 100;
    int ef_search = 16;

    hnswlib::InnerProductSpace space(dim);

    hnswlib::HierarchicalNSW<float> index(&space, max_elements, M, ef_construction);

    std::vector<float> v1 = {1.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> v2 = {0.0f, 1.0f, 0.0f, 0.0f};
    std::vector<float> v3 = {0.0f, 0.0f, 1.0f, 0.0f};

    index.addPoint(v1.data(), 101);
    index.addPoint(v2.data(), 103);
    index.addPoint(v3.data(), 105);

    index.setEf(ef_search);

    std::vector<float> query = {0.95f, 0.1f, 0.05f, 0.0f};

    // searchKnn returns priority_queue (further-first), pop top = nearest
    auto result = index.searchKnn(query.data(), 1);
    auto top_label = result.top().second;
    auto top_distance = result.top().first;

    std::cout << "top label: " << top_label << std::endl;
    std::cout << "distance: " << top_distance << std::endl;

    if (top_label == 101) {
        std::cout << "hnswlib smoke test passed" << std::endl;
        return 0;
    } else {
        std::cerr << "FAIL: expected label 101, got " << top_label << std::endl;
        return 1;
    }
}
