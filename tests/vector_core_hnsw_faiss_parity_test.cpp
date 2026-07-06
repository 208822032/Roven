#include <faiss/IndexHNSW.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/time.h>
#include <vector>

#include "vector_core_hnsw_faiss_parity_bridge.h"

#define P1B_DIM 512
#define P1B_TOPK 10
#define P1B_BASE_RECORD_ID 1000000ULL
#define P1B_MAX_EFS 3

struct ExactItem {
	uint64_t record_id;
	float score;
};

struct Dataset {
	size_t count = 0;
	size_t queries = 0;
	std::vector<float> base;
	std::vector<float> query;
	std::vector<ExactItem> exact;
	std::vector<uint64_t> exact_ids;
	std::vector<size_t> exact_counts;
	size_t oracle_mismatch_count = 0;
	uint64_t oracle_us = 0;
};

struct Profile {
	const char *name;
	size_t M;
	size_t M0;
	size_t ef_construction;
	size_t efs[P1B_MAX_EFS];
	size_t ef_count;
};

struct BackendResult {
	std::string backend;
	std::string profile;
	size_t count = 0;
	size_t queries = 0;
	size_t M = 0;
	size_t M0 = 0;
	size_t ef_construction = 0;
	size_t ef_search = 0;
	uint64_t build_us = 0;
	double search_avg_us = 0.0;
	uint64_t search_p50_us = 0;
	uint64_t search_p95_us = 0;
	double recall = 0.0;
	size_t memory_bytes = 0;
	std::string memory_source = "unsupported";
	double visited_avg = 0.0;
	double distance_avg = 0.0;
	uint64_t duplicate_edge = 0;
	uint64_t degree_overflow = 0;
};

static uint64_t now_us(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static long rss_kb(void) {
	struct rusage usage;

	if (getrusage(RUSAGE_SELF, &usage) == 0) {
		return usage.ru_maxrss;
	}
	return -1;
}

static uint32_t rng_next(uint64_t *state) {
	uint64_t z;

	*state += 0x9e3779b97f4a7c15ULL;
	z = *state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	z ^= z >> 31;
	return (uint32_t)(z >> 32);
}

static void fill_random_unit_vector(float *vector, size_t dim, uint64_t seed) {
	uint64_t state = seed;
	float norm = 0.0f;

	for (size_t i = 0; i < dim; i++) {
		uint32_t raw = rng_next(&state);
		float x = (float)((int)(raw % 20001u) - 10000) / 10000.0f;
		vector[i] = x;
		norm += x * x;
	}

	if (norm > 0.0f) {
		float inv = 1.0f / std::sqrt(norm);
		for (size_t i = 0; i < dim; i++) {
			vector[i] *= inv;
		}
	}
}

static float ip_score(const float *lhs, const float *rhs, size_t dim) {
	float score = 0.0f;

	for (size_t i = 0; i < dim; i++) {
		score += lhs[i] * rhs[i];
	}
	return score;
}

static bool exact_better(float lhs_score, uint64_t lhs_id, float rhs_score, uint64_t rhs_id) {
	if (lhs_score > rhs_score) {
		return true;
	}
	if (lhs_score < rhs_score) {
		return false;
	}
	return lhs_id < rhs_id;
}

static void direct_exact_topk(
	const float *base,
	size_t count,
	const float *query,
	ExactItem *out,
	size_t *out_count
) {
	size_t n = 0;

	for (size_t i = 0; i < count; i++) {
		float score = ip_score(query, base + i * P1B_DIM, P1B_DIM);
		uint64_t record_id = P1B_BASE_RECORD_ID + (uint64_t)i;

		if (n == P1B_TOPK &&
		    !exact_better(score, record_id, out[n - 1].score, out[n - 1].record_id)) {
			continue;
		}

		if (n < P1B_TOPK) {
			n++;
		}
		size_t pos = n - 1;
		while (pos > 0 &&
		       exact_better(score, record_id, out[pos - 1].score, out[pos - 1].record_id)) {
			out[pos] = out[pos - 1];
			pos--;
		}
		out[pos].record_id = record_id;
		out[pos].score = score;
	}

	*out_count = n;
}

static bool scores_descending(const std::vector<float> &scores, size_t count) {
	for (size_t i = 1; i < count; i++) {
		if (scores[i - 1] < scores[i]) {
			return false;
		}
	}
	return true;
}

static double recall_record_ids(
	const ExactItem *exact,
	size_t exact_count,
	const std::vector<uint64_t> &record_ids
) {
	size_t matched = 0;

	if (exact_count == 0) {
		return 1.0;
	}

	for (size_t i = 0; i < exact_count; i++) {
		for (uint64_t record_id : record_ids) {
			if (record_id == exact[i].record_id) {
				matched++;
				break;
			}
		}
	}
	return (double)matched / (double)exact_count;
}

static uint64_t percentile_u64(std::vector<uint64_t> values, double pct) {
	if (values.empty()) {
		return 0;
	}
	std::sort(values.begin(), values.end());
	size_t idx = (size_t)((pct / 100.0) * (double)values.size());
	if (idx >= values.size()) {
		idx = values.size() - 1;
	}
	return values[idx];
}

static double avg_u64(const std::vector<uint64_t> &values) {
	uint64_t total = 0;

	if (values.empty()) {
		return 0.0;
	}
	for (uint64_t value : values) {
		total += value;
	}
	return (double)total / (double)values.size();
}

static int dataset_init(Dataset *ds, size_t count, size_t queries) {
	uint64_t start_us;
	int failed = 0;

	ds->count = count;
	ds->queries = queries;
	ds->base.resize(count * P1B_DIM);
	ds->query.resize(queries * P1B_DIM);
	ds->exact.resize(queries * P1B_TOPK);
	ds->exact_ids.assign(queries * P1B_TOPK, 0);
	ds->exact_counts.assign(queries, 0);
	ds->oracle_mismatch_count = 0;
	ds->oracle_us = 0;

	for (size_t i = 0; i < count; i++) {
		fill_random_unit_vector(
			ds->base.data() + i * P1B_DIM,
			P1B_DIM,
			0xabc00000ULL + (uint64_t)i
		);
	}
	for (size_t i = 0; i < queries; i++) {
		fill_random_unit_vector(
			ds->query.data() + i * P1B_DIM,
			P1B_DIM,
			0xdad00000ULL + (uint64_t)i
		);
	}

	start_us = now_us();
	for (size_t i = 0; i < queries; i++) {
		size_t exact_count = 0;

		direct_exact_topk(
			ds->base.data(),
			count,
			ds->query.data() + i * P1B_DIM,
			ds->exact.data() + i * P1B_TOPK,
			&exact_count
		);
		ds->exact_counts[i] = exact_count;
		for (size_t j = 0; j < exact_count; j++) {
			ds->exact_ids[i * P1B_TOPK + j] = ds->exact[i * P1B_TOPK + j].record_id;
		}
	}
	if (p1b_bridge_flat_crosscheck(
		    count,
		    P1B_DIM,
		    P1B_TOPK,
		    P1B_BASE_RECORD_ID,
		    ds->base.data(),
		    queries,
		    ds->query.data(),
		    ds->exact_ids.data(),
		    ds->exact_counts.data(),
		    &ds->oracle_mismatch_count
	    ) != 0) {
		std::fprintf(stderr, "hnsw_p1b_core_flat_crosscheck_failed count=%zu\n", count);
		failed = 1;
	}
	ds->oracle_us = now_us() - start_us;

	std::printf(
		"hnsw_p1b_oracle count=%zu dim=%u topk=%u queries=%zu "
		"metric=ip_normalized direct_exact=1 core_flat_crosscheck=1 "
		"mismatch=%zu oracle_us=%llu\n",
		count,
		P1B_DIM,
		P1B_TOPK,
		queries,
		ds->oracle_mismatch_count,
		(unsigned long long)ds->oracle_us
	);
	if (ds->oracle_mismatch_count != 0) {
		failed = 1;
	}

	return failed;
}

static void print_result(const BackendResult &r) {
	if (r.memory_source == "measured") {
		std::printf(
			"hnsw_p1b_result backend=%s count=%zu dim=%u topk=%u queries=%zu "
			"profile=%s M=%zu M0=%zu efConstruction=%zu efSearch=%zu "
			"build_us=%llu search_avg_us=%.2f search_p50_us=%llu "
			"search_p95_us=%llu recall@10=%.4f memory_bytes=%zu "
			"memory_source=%s visited_avg=%.2f distance_avg=%.2f "
			"duplicate_edge=%llu degree_overflow=%llu\n",
			r.backend.c_str(),
			r.count,
			P1B_DIM,
			P1B_TOPK,
			r.queries,
			r.profile.c_str(),
			r.M,
			r.M0,
			r.ef_construction,
			r.ef_search,
			(unsigned long long)r.build_us,
			r.search_avg_us,
			(unsigned long long)r.search_p50_us,
			(unsigned long long)r.search_p95_us,
			r.recall,
			r.memory_bytes,
			r.memory_source.c_str(),
			r.visited_avg,
			r.distance_avg,
			(unsigned long long)r.duplicate_edge,
			(unsigned long long)r.degree_overflow
		);
	} else {
		std::printf(
			"hnsw_p1b_result backend=%s count=%zu dim=%u topk=%u queries=%zu "
			"profile=%s M=%zu M0=%zu efConstruction=%zu efSearch=%zu "
			"build_us=%llu search_avg_us=%.2f search_p50_us=%llu "
			"search_p95_us=%llu recall@10=%.4f memory_bytes=N/A "
			"memory_source=%s visited_avg=N/A distance_avg=N/A "
			"duplicate_edge=N/A degree_overflow=N/A process_rss_kb=%ld\n",
			r.backend.c_str(),
			r.count,
			P1B_DIM,
			P1B_TOPK,
			r.queries,
			r.profile.c_str(),
			r.M,
			r.M0,
			r.ef_construction,
			r.ef_search,
			(unsigned long long)r.build_us,
			r.search_avg_us,
			(unsigned long long)r.search_p50_us,
			(unsigned long long)r.search_p95_us,
			r.recall,
			r.memory_source.c_str(),
			rss_kb()
		);
	}
}

static int run_v2_profile(
	const Dataset &ds,
	const Profile &profile,
	std::vector<BackendResult> *results
) {
	p1b_bridge_graph_t graph;
	p1b_bridge_result_t bridge_results[P1B_MAX_EFS];
	uint64_t seed = 0x9d000000ULL + (uint64_t)ds.count +
		(uint64_t)profile.M * 17ULL + (uint64_t)profile.ef_construction * 31ULL;

	std::printf(
		"hnsw_p1b_build_start backend=core_hnsw_v2 count=%zu profile=%s "
		"M=%zu M0=%zu efConstruction=%zu\n",
		ds.count,
		profile.name,
		profile.M,
		profile.M0,
		profile.ef_construction
	);
	std::fflush(stdout);

	memset(&graph, 0, sizeof(graph));
	memset(bridge_results, 0, sizeof(bridge_results));
	if (p1b_bridge_run_v2_profile(
		    ds.count,
		    P1B_DIM,
		    P1B_TOPK,
		    P1B_BASE_RECORD_ID,
		    ds.base.data(),
		    ds.queries,
		    ds.query.data(),
		    ds.exact_ids.data(),
		    ds.exact_counts.data(),
		    profile.M,
		    profile.M0,
		    profile.ef_construction,
		    profile.efs,
		    profile.ef_count,
		    seed,
		    &graph,
		    bridge_results
	    ) != 0) {
		std::fprintf(stderr, "hnsw_p1b_v2_profile_failed count=%zu profile=%s\n",
			ds.count, profile.name);
		return 1;
	}

	std::printf(
		"hnsw_p1b_graph backend=core_hnsw_v2 count=%zu profile=%s "
		"build_us=%llu memory_bytes=%zu max_level=%u entry_point_level=%u "
		"level0_degree_avg=%.2f upper_degree_avg=%.2f isolated=%llu "
		"duplicate_edge=%llu degree_overflow=%llu selected=%llu reverse_prune=%llu\n",
		ds.count,
		profile.name,
		(unsigned long long)bridge_results[0].build_us,
		graph.memory_bytes,
		(unsigned)graph.max_level,
		(unsigned)graph.entry_point_level,
		graph.level0_degree_avg,
		graph.upper_degree_avg,
		(unsigned long long)graph.isolated_node_count,
		(unsigned long long)graph.duplicate_edge_count,
		(unsigned long long)graph.degree_overflow_count,
		(unsigned long long)graph.selected_neighbor_count,
		(unsigned long long)graph.reverse_prune_count
	);

	for (size_t ef_idx = 0; ef_idx < profile.ef_count; ef_idx++) {
		size_t ef = profile.efs[ef_idx];

		BackendResult result;
		result.backend = "core_hnsw_v2";
		result.profile = profile.name;
		result.count = ds.count;
		result.queries = ds.queries;
		result.M = profile.M;
		result.M0 = profile.M0;
		result.ef_construction = profile.ef_construction;
		result.ef_search = ef;
		result.build_us = bridge_results[ef_idx].build_us;
		result.search_avg_us = bridge_results[ef_idx].search_avg_us;
		result.search_p50_us = bridge_results[ef_idx].search_p50_us;
		result.search_p95_us = bridge_results[ef_idx].search_p95_us;
		result.recall = bridge_results[ef_idx].recall;
		result.memory_bytes = bridge_results[ef_idx].memory_bytes;
		result.memory_source = "measured";
		result.visited_avg = bridge_results[ef_idx].visited_avg;
		result.distance_avg = bridge_results[ef_idx].distance_avg;
		result.duplicate_edge = bridge_results[ef_idx].duplicate_edge;
		result.degree_overflow = bridge_results[ef_idx].degree_overflow;
		results->push_back(result);
		print_result(result);
	}
	return 0;
}

static int run_faiss_profile(
	const Dataset &ds,
	const Profile &profile,
	std::vector<BackendResult> *results
) {
	std::vector<uint64_t> elapsed(ds.queries);
	std::vector<float> distances(P1B_TOPK);
	std::vector<faiss::idx_t> labels(P1B_TOPK);
	uint64_t build_start_us;
	uint64_t build_us = 0;
	int failed = 0;

	try {
		faiss::IndexHNSWFlat index((int)P1B_DIM, (int)profile.M, faiss::METRIC_INNER_PRODUCT);
		index.hnsw.efConstruction = (int)profile.ef_construction;

		std::printf(
			"hnsw_p1b_build_start backend=faiss_hnsw count=%zu profile=%s "
			"M=%zu M0=N/A efConstruction=%zu\n",
			ds.count,
			profile.name,
			profile.M,
			profile.ef_construction
		);
		std::fflush(stdout);

		build_start_us = now_us();
		for (size_t i = 0; i < ds.count; i++) {
			index.add(1, ds.base.data() + i * P1B_DIM);
		}
		build_us = now_us() - build_start_us;

		for (size_t ef_idx = 0; ef_idx < profile.ef_count; ef_idx++) {
			size_t ef = profile.efs[ef_idx];
			double recall_sum = 0.0;

			index.hnsw.efSearch = (int)ef;
			for (size_t q = 0; q < ds.queries; q++) {
				std::vector<uint64_t> record_ids;
				uint64_t start_us = now_us();

				index.search(
					1,
					ds.query.data() + q * P1B_DIM,
					(faiss::idx_t)P1B_TOPK,
					distances.data(),
					labels.data()
				);
				elapsed[q] = now_us() - start_us;
				if (!scores_descending(distances, P1B_TOPK)) {
					std::fprintf(stderr,
						"hnsw_p1b_faiss_non_descending count=%zu profile=%s ef=%zu q=%zu\n",
						ds.count, profile.name, ef, q);
					failed = 1;
					return failed;
				}
				record_ids.reserve(P1B_TOPK);
				for (size_t j = 0; j < P1B_TOPK; j++) {
					if (labels[j] >= 0) {
						record_ids.push_back(P1B_BASE_RECORD_ID + (uint64_t)labels[j]);
					}
				}
				recall_sum += recall_record_ids(
					ds.exact.data() + q * P1B_TOPK,
					ds.exact_counts[q],
					record_ids
				);
			}

			BackendResult result;
			result.backend = "faiss_hnsw";
			result.profile = profile.name;
			result.count = ds.count;
			result.queries = ds.queries;
			result.M = profile.M;
			result.M0 = 0;
			result.ef_construction = profile.ef_construction;
			result.ef_search = ef;
			result.build_us = build_us;
			result.search_avg_us = avg_u64(elapsed);
			result.search_p50_us = percentile_u64(elapsed, 50.0);
			result.search_p95_us = percentile_u64(elapsed, 95.0);
			result.recall = recall_sum / (double)ds.queries;
			result.memory_source = "unsupported";
			results->push_back(result);
			print_result(result);
		}
	} catch (const std::exception &e) {
		std::fprintf(stderr, "hnsw_p1b_faiss_exception count=%zu profile=%s error=%s\n",
			ds.count, profile.name, e.what());
		failed = 1;
	}

	return failed;
}

static const BackendResult *find_result(
	const std::vector<BackendResult> &results,
	const char *backend,
	const char *profile,
	size_t ef
) {
	for (const BackendResult &result : results) {
		if (result.backend == backend &&
		    result.profile == profile &&
		    result.ef_search == ef) {
			return &result;
		}
	}
	return NULL;
}

static void print_gaps(const std::vector<BackendResult> &results, const Profile &profile) {
	for (size_t ef_idx = 0; ef_idx < profile.ef_count; ef_idx++) {
		size_t ef = profile.efs[ef_idx];
		const BackendResult *v2 = find_result(results, "core_hnsw_v2", profile.name, ef);
		const BackendResult *faiss = find_result(results, "faiss_hnsw", profile.name, ef);
		if (!v2 || !faiss) {
			continue;
		}
		double gap = faiss->recall - v2->recall;
		std::printf(
			"hnsw_p1b_gap count=%zu profile=%s efSearch=%zu "
			"v2_recall=%.4f faiss_recall=%.4f faiss_minus_v2=%.4f "
			"significant=%s threshold=0.0300\n",
			v2->count,
			profile.name,
			ef,
			v2->recall,
			faiss->recall,
			gap,
			(gap >= 0.03) ? "yes" : "no"
		);
	}
}

static size_t env_size_or(const char *name, size_t fallback) {
	const char *value = std::getenv(name);
	char *end = NULL;
	unsigned long long parsed;

	if (!value || !*value) {
		return fallback;
	}
	parsed = std::strtoull(value, &end, 10);
	if (!end || *end != '\0' || parsed == 0ULL) {
		return fallback;
	}
	return (size_t)parsed;
}

static bool mode_is(const char *mode, const char *expected) {
	return mode && std::strcmp(mode, expected) == 0;
}

static int run_case(
	size_t count,
	size_t queries,
	const char *mode,
	const Profile *profiles,
	size_t profile_count
) {
	Dataset ds;
	std::vector<BackendResult> results;
	int failed = 0;

	if (dataset_init(&ds, count, queries) != 0) {
		return 1;
	}

	std::printf(
		"hnsw_p1b_case_start mode=%s count=%zu dim=%u topk=%u queries=%zu "
		"profile_count=%zu backends=core_hnsw_v2,faiss_hnsw metric=ip_normalized\n",
		mode,
		count,
		P1B_DIM,
		P1B_TOPK,
		queries,
		profile_count
	);

	for (size_t i = 0; i < profile_count; i++) {
		failed |= run_v2_profile(ds, profiles[i], &results);
		if (failed) {
			break;
		}
		failed |= run_faiss_profile(ds, profiles[i], &results);
		if (failed) {
			break;
		}
		print_gaps(results, profiles[i]);
	}

	return failed;
}

int main(void) {
	static const Profile baseline_10k = {
		"baseline",
		32,
		64,
		256,
		{256, 512, 0},
		2
	};
	static const Profile baseline_50k = {
		"baseline",
		32,
		64,
		256,
		{512, 1024, 0},
		2
	};
	static const Profile balanced = {
		"balanced",
		48,
		96,
		512,
		{1024, 0, 0},
		1
	};
	const char *mode = std::getenv("HNSW_P1B_MODE");
	int failed = 0;

	if (!mode || !*mode) {
		mode = "quick";
	}

	if (p1b_bridge_init_runtime() != 0) {
		std::fprintf(stderr, "hnsw_p1b_bridge_init_failed\n");
		return 1;
	}

	std::printf(
		"hnsw_p1b_start mode=%s metric=ip_normalized dim=%u topk=%u seed=fixed "
		"faiss_direct=1 faiss_efSearch_setter=1 quick=10K_sanity_plus_50K_baseline "
		"balanced_only=50K_balanced "
		"full=10K_sanity_plus_50K_baseline_balanced optional_100K=HNSW_P1B_100K=1\n",
		mode,
		P1B_DIM,
		P1B_TOPK
	);

	if (mode_is(mode, "smoke")) {
		Profile smoke = baseline_10k;
		smoke.efs[0] = 128;
		smoke.efs[1] = 256;
		smoke.ef_count = 2;
		failed |= run_case(
			env_size_or("HNSW_P1B_SMOKE_COUNT", 2000),
			env_size_or("HNSW_P1B_SMOKE_QUERIES", 20),
			mode,
			&smoke,
			1
		);
	} else if (mode_is(mode, "quick")) {
		failed |= run_case(
			env_size_or("HNSW_P1B_10K_COUNT", 10000),
			env_size_or("HNSW_P1B_10K_QUERIES", 100),
			mode,
			&baseline_10k,
			1
		);
		if (!failed) {
			failed |= run_case(
				env_size_or("HNSW_P1B_50K_COUNT", 50000),
				env_size_or("HNSW_P1B_50K_QUERIES", 200),
				mode,
				&baseline_50k,
				1
			);
		}
	} else if (mode_is(mode, "balanced-only")) {
		failed |= run_case(
			env_size_or("HNSW_P1B_50K_COUNT", 50000),
			env_size_or("HNSW_P1B_50K_QUERIES", 200),
			mode,
			&balanced,
			1
		);
	} else if (mode_is(mode, "full")) {
		const Profile profiles_50k[] = {baseline_50k, balanced};

		failed |= run_case(
			env_size_or("HNSW_P1B_10K_COUNT", 10000),
			env_size_or("HNSW_P1B_10K_QUERIES", 100),
			mode,
			&baseline_10k,
			1
		);
		if (!failed) {
			failed |= run_case(
				env_size_or("HNSW_P1B_50K_COUNT", 50000),
				env_size_or("HNSW_P1B_50K_QUERIES", 200),
				mode,
				profiles_50k,
				sizeof(profiles_50k) / sizeof(profiles_50k[0])
			);
		}
		if (!failed) {
			const char *run_100k = std::getenv("HNSW_P1B_100K");
			if (run_100k && std::strcmp(run_100k, "1") == 0) {
				Profile smoke_100k = balanced;
				failed |= run_case(
					env_size_or("HNSW_P1B_100K_COUNT", 100000),
					env_size_or("HNSW_P1B_100K_QUERIES", 50),
					mode,
					&smoke_100k,
					1
				);
			} else {
				std::printf(
					"hnsw_p1b_100k_status status=not_run "
					"reason=optional_set_HNSW_P1B_100K_1\n"
				);
			}
		}
	} else {
		std::fprintf(stderr,
			"unknown HNSW_P1B_MODE=%s (expected smoke, quick, balanced-only, full)\n",
			mode);
		failed = 1;
	}

	if (failed) {
		std::fprintf(stderr, "vector_core_hnsw_faiss_parity_test FAILED\n");
		return 1;
	}
	std::printf("vector_core_hnsw_faiss_parity_test PASS\n");
	return 0;
}
