#!/usr/bin/env bash
# B-scale-0 T2Ranking subset generator smoke test.
#
# Uses only small synthetic fixtures. It verifies deterministic output,
# source-row-order independence, positive retention, collection nesting,
# ids/TSV order consistency, and key failure paths. It does not inspect the
# real T2Ranking dataset.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

GEN="scripts/benchmark/generate_t2ranking_subsets.py"
TMP_ROOT="/tmp/kvlite_t2ranking_subset_smoke_$$_$(date +%s)"
REVISION="smoke-revision"

cleanup() { rm -rf "${TMP_ROOT:?}"; }
trap cleanup EXIT

pass() { printf '[PASS] %s\n' "$1"; }
fail() { printf '[FAIL] %s\n' "$1" >&2; exit 1; }

expect_fail() {
	local name="$1"
	shift
	if "$@" >"$TMP_ROOT/${name}.out" 2>"$TMP_ROOT/${name}.err"; then
		fail "$name unexpectedly succeeded"
	fi
	pass "$name failed as expected"
}

write_fixture_ordered() {
	local dir="$1"
	mkdir -p "$dir"
	cat >"$dir/collection.tsv" <<'EOF'
pid	text
p1	doc one
p2	doc two
p3	doc three
p4	doc four
p5	doc five
p6	doc six
p7	doc seven
p8	doc eight
p9	doc nine
p10	doc ten
p11	doc eleven
p12	doc twelve
EOF
	cat >"$dir/queries.dev.tsv" <<'EOF'
qid	text
q1	query one
q2	query two
q3	query three
q4	query four
q5	query five
q6	query six
EOF
	cat >"$dir/qrels.retrieval.dev.tsv" <<'EOF'
qid	pid
q1	p1
q1	p2
q2	p3
q3	p4
q4	p5
q5	p6
q6	p7
EOF
}

write_fixture_shuffled() {
	local dir="$1"
	mkdir -p "$dir"
	cat >"$dir/collection.tsv" <<'EOF'
pid	text
p12	doc twelve
p3	doc three
p1	doc one
p8	doc eight
p6	doc six
p10	doc ten
p2	doc two
p11	doc eleven
p7	doc seven
p5	doc five
p9	doc nine
p4	doc four
EOF
	cat >"$dir/queries.dev.tsv" <<'EOF'
qid	text
q6	query six
q3	query three
q1	query one
q5	query five
q2	query two
q4	query four
EOF
	cat >"$dir/qrels.retrieval.dev.tsv" <<'EOF'
qid	pid
q4	p5
q1	p2
q6	p7
q2	p3
q5	p6
q1	p1
q3	p4
EOF
}

tree_checksums() {
	local dir="$1"
	(
		cd "$dir"
		find . -type f -print0 | sort -z | xargs -0 sha256sum
	)
}

mkdir -p "$TMP_ROOT"
FIX_A="$TMP_ROOT/fixture_a"
FIX_B="$TMP_ROOT/fixture_b"
OUT_A="$TMP_ROOT/out_a"
OUT_B="$TMP_ROOT/out_b"
write_fixture_ordered "$FIX_A"
write_fixture_shuffled "$FIX_B"

python3 "$GEN" \
	--dataset-root "$FIX_A" \
	--output-root "$OUT_A" \
	--source-revision "$REVISION" \
	--query-count 6 \
	--collection-sizes 8,10 >"$TMP_ROOT/out_a.json"

python3 "$GEN" \
	--dataset-root "$FIX_B" \
	--output-root "$OUT_B" \
	--source-revision "$REVISION" \
	--query-count 6 \
	--collection-sizes 8,10 >"$TMP_ROOT/out_b.json"

diff -ru "$OUT_A" "$OUT_B" >"$TMP_ROOT/diff.txt"
pass "source row order does not affect output"

before="$(tree_checksums "$OUT_A")"
python3 "$GEN" \
	--dataset-root "$FIX_A" \
	--output-root "$OUT_A" \
	--source-revision "$REVISION" \
	--query-count 6 \
	--collection-sizes 8,10 >"$TMP_ROOT/repeat.json"
after="$(tree_checksums "$OUT_A")"
[[ "$before" == "$after" ]] || fail "repeated run changed output checksums"
grep -q '"status": "unchanged"' "$TMP_ROOT/repeat.json" || fail "repeat run did not report unchanged"
pass "repeated run is deterministic"

python3 - "$OUT_A" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])

with (root / "manifest.json").open(encoding="utf-8") as f:
    manifest = json.load(f)
assert manifest["schema_version"] == 1
assert manifest["source_revision"] == "smoke-revision"
assert manifest["counts"]["selected_query_count"] == 6
assert manifest["counts"]["positive_pair_count"] == 7
assert manifest["counts"]["unique_positive_pid_count"] == 7
assert manifest["verification"]["nested_prefix"] is True
assert manifest["verification"]["positive_pids_present_in_all_collections"] is True
assert manifest["verification"]["ids_match_tsv_order"] is True

def ids_file(name):
    return [line.rstrip("\n") for line in (root / name).open(encoding="utf-8")]

def tsv_ids(name):
    with (root / name).open(encoding="utf-8") as f:
        header = f.readline().rstrip("\n")
        assert header == "pid\ttext", header
        return [line.rstrip("\n").split("\t", 1)[0] for line in f]

small = ids_file("collection.8.ids")
large = ids_file("collection.10.ids")
assert len(small) == 8
assert len(large) == 10
assert small == large[:8]
assert small == tsv_ids("collection.8.tsv")
assert large == tsv_ids("collection.10.tsv")

positives = set()
with (root / "qrels.retrieval.dev.6.tsv").open(encoding="utf-8") as f:
    assert f.readline().rstrip("\n") == "qid\tpid"
    for line in f:
        qid, pid = line.rstrip("\n").split("\t")
        positives.add(pid)
assert positives == {"p1", "p2", "p3", "p4", "p5", "p6", "p7"}
assert positives.issubset(set(small))
assert positives.issubset(set(large))
print("[python] manifest and output invariants passed")
PY
pass "positive retention, nesting, and id order validated"

BAD_DUP_PID="$TMP_ROOT/bad_dup_pid"
cp -r "$FIX_A" "$BAD_DUP_PID"
printf 'p1\tduplicate pid\n' >>"$BAD_DUP_PID/collection.tsv"
expect_fail duplicate_pid python3 "$GEN" \
	--dataset-root "$BAD_DUP_PID" \
	--output-root "$TMP_ROOT/out_bad_dup_pid" \
	--source-revision "$REVISION" \
	--query-count 6 \
	--collection-sizes 8,10

BAD_DUP_QID="$TMP_ROOT/bad_dup_qid"
cp -r "$FIX_A" "$BAD_DUP_QID"
printf 'q1\tduplicate query\n' >>"$BAD_DUP_QID/queries.dev.tsv"
expect_fail duplicate_qid python3 "$GEN" \
	--dataset-root "$BAD_DUP_QID" \
	--output-root "$TMP_ROOT/out_bad_dup_qid" \
	--source-revision "$REVISION" \
	--query-count 6 \
	--collection-sizes 8,10

BAD_HEADER="$TMP_ROOT/bad_header"
cp -r "$FIX_A" "$BAD_HEADER"
sed -i '1s/.*/bad\theader/' "$BAD_HEADER/collection.tsv"
expect_fail bad_header python3 "$GEN" \
	--dataset-root "$BAD_HEADER" \
	--output-root "$TMP_ROOT/out_bad_header" \
	--source-revision "$REVISION" \
	--query-count 6 \
	--collection-sizes 8,10

expect_fail insufficient_data python3 "$GEN" \
	--dataset-root "$FIX_A" \
	--output-root "$TMP_ROOT/out_insufficient" \
	--source-revision "$REVISION" \
	--query-count 6 \
	--collection-sizes 8,20

printf '[PASS] vector_t2ranking_subset_generator_smoke_test completed\n'
