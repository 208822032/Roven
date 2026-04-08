#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/kvs_ai_chat_schema.h"
#include "../include/kvs_globals.h"
#include "../include/kvs_memory_internal.h"

static void require_int(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "ai_chat_schema_test failed: %s\n", message);
        exit(1);
    }
}

static void require_str_eq(const char *lhs, const char *rhs, const char *message) {
    if (!lhs || !rhs || strcmp(lhs, rhs) != 0) {
        fprintf(stderr, "ai_chat_schema_test failed: %s\n", message);
        fprintf(stderr, "  lhs=%s\n", lhs ? lhs : "(null)");
        fprintf(stderr, "  rhs=%s\n", rhs ? rhs : "(null)");
        exit(1);
    }
}

int main(void) {
    kvs_hash_t qa_store;
    kvs_skiptable_t candidate_index;
    char buffer[1024];

    memset(&qa_store, 0, sizeof(qa_store));
    memset(&candidate_index, 0, sizeof(candidate_index));

    g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
    mem_init();

    require_int(kvs_hash_create(&qa_store) == 0, "kvs_hash_create");
    require_int(kvs_skiptable_create(&candidate_index) == 0, "kvs_skiptable_create");

    const char *record_id = "rec-001";
    const char *session_id = "sess-001";
    const char *question_hash = "abc123";
    const char *scope = "provider=openai|model=gpt-4o-mini|session=sess-001";

    const char *qa_json =
        "{"
        "\"record_id\":\"rec-001\"," 
        "\"session_id\":\"sess-001\"," 
        "\"turn_seq\":7," 
        "\"question\":\"what is kvlite\"," 
        "\"normalized_question\":\"what is kvlite\"," 
        "\"question_hash\":\"abc123\"," 
        "\"answer\":\"KVLite is a lightweight KV engine\"," 
        "\"source_type\":\"hybrid\"," 
        "\"provider\":\"openai\"," 
        "\"model\":\"gpt-4o-mini\"," 
        "\"embedding_model\":\"text-embedding-3-small\"," 
        "\"embedding_status\":\"ready\"," 
        "\"created_at\":1710000000," 
        "\"updated_at\":1710000001"
        "}";

    const char *qnorm_json =
        "{"
        "\"question_hash\":\"abc123\"," 
        "\"scope\":\"global\"," 
        "\"candidates\":[\"rec-001\",\"rec-002\"]"
        "}";

    const char *qnorm_scope_json =
        "{"
        "\"question_hash\":\"abc123\"," 
        "\"scope\":\"provider=openai|model=gpt-4o-mini|session=sess-001\"," 
        "\"candidates\":[\"rec-001\"]"
        "}";

    const char *embedding_json =
        "{"
        "\"record_id\":\"rec-001\"," 
        "\"status\":\"ready\"," 
        "\"mode\":\"blob\"," 
        "\"blob_b64\":\"AQIDBA==\"," 
        "\"dim\":4"
        "}";

    require_int(kvs_ai_chat_build_qa_key(buffer, sizeof(buffer), record_id) > 0, "build qa key");
    require_str_eq(buffer, "qa:rec-001", "qa key format");

    require_int(kvs_ai_chat_build_qnorm_key(buffer, sizeof(buffer), question_hash) > 0, "build qnorm key");
    require_str_eq(buffer, "qnorm:abc123", "qnorm key format");

    require_int(kvs_ai_chat_build_qnorm_scope_key(buffer, sizeof(buffer), question_hash, scope) > 0, "build qnorm scope key");
    require_str_eq(buffer, "qnorm:abc123:scope:provider=openai|model=gpt-4o-mini|session=sess-001", "qnorm scope key format");

    require_int(kvs_ai_chat_build_session_turn_key(buffer, sizeof(buffer), session_id, 7) > 0, "build session turn key");
    require_str_eq(buffer, "sess:sess-001:turn:0000000007", "session turn key format");

    require_int(kvs_ai_chat_build_embedding_key(buffer, sizeof(buffer), record_id) > 0, "build embedding key");
    require_str_eq(buffer, "vec:rec-001", "embedding key format");

    require_int(kvs_ai_chat_build_candidate_key(buffer, sizeof(buffer), scope, question_hash, record_id) > 0, "build candidate key");
    require_str_eq(buffer, "cand:provider=openai|model=gpt-4o-mini|session=sess-001:abc123:rec-001", "candidate key format");

    require_int(kvs_ai_chat_put_qa_record(&qa_store, record_id, qa_json) >= 0, "put qa record");
    require_int(kvs_ai_chat_put_qnorm_index(&qa_store, question_hash, qnorm_json) >= 0, "put qnorm index");
    require_int(kvs_ai_chat_put_qnorm_scope_index(&qa_store, question_hash, scope, qnorm_scope_json) >= 0, "put qnorm scope index");
    require_int(kvs_ai_chat_put_session_turn(&qa_store, session_id, 7, record_id) >= 0, "put session turn");
    require_int(kvs_ai_chat_put_embedding(&qa_store, record_id, embedding_json) >= 0, "put embedding");
    require_int(kvs_ai_chat_put_candidate(&candidate_index, scope, question_hash, record_id, record_id) >= 0, "put candidate");

    require_str_eq(kvs_ai_chat_load_qa_record(&qa_store, record_id), qa_json, "load qa record");
    require_str_eq(kvs_ai_chat_load_qnorm_index(&qa_store, question_hash), qnorm_json, "load qnorm index");
    require_str_eq(kvs_ai_chat_load_qnorm_scope_index(&qa_store, question_hash, scope), qnorm_scope_json, "load qnorm scope index");
    require_str_eq(kvs_ai_chat_load_session_turn(&qa_store, session_id, 7), record_id, "load session turn mapping");
    require_str_eq(kvs_ai_chat_load_record_by_session_turn(&qa_store, session_id, 7), qa_json, "load record by session turn");
    require_str_eq(kvs_ai_chat_load_embedding(&qa_store, record_id), embedding_json, "load embedding");

    memset(buffer, 0, sizeof(buffer));
    require_int(kvs_ai_chat_scan_candidates(&candidate_index, scope, 10, buffer, sizeof(buffer)) > 0, "scan candidates");
    require_int(strstr(buffer, "cand:provider=openai|model=gpt-4o-mini|session=sess-001:abc123:rec-001") != NULL, "candidate scan contains key");
    require_int(strstr(buffer, record_id) != NULL, "candidate scan contains record id");

    kvs_skiptable_destroy(&candidate_index);
    kvs_hash_destory(&qa_store);
    mem_destroy();

    puts("ai_chat_schema_test PASSED");
    return 0;
}
