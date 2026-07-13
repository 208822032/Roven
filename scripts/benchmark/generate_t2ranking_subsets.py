#!/usr/bin/env python3
"""Generate deterministic T2Ranking subsets for real-embedding scale benchmarks."""

from __future__ import annotations

import argparse
import hashlib
import heapq
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Set, Tuple


ALGORITHM_VERSION = "t2ranking-subset-v1"

COLLECTION_HEADER = ("pid", "text")
QUERIES_HEADER = ("qid", "text")
QRELS_HEADER = ("qid", "pid")


class SubsetError(RuntimeError):
    pass


def parse_sizes(value: str) -> List[int]:
    try:
        sizes = [int(part.strip()) for part in value.split(",") if part.strip()]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("--collection-sizes must be comma-separated integers") from exc
    if not sizes:
        raise argparse.ArgumentTypeError("--collection-sizes must not be empty")
    if any(size <= 0 for size in sizes):
        raise argparse.ArgumentTypeError("--collection-sizes must be positive")
    if len(set(sizes)) != len(sizes):
        raise argparse.ArgumentTypeError("--collection-sizes must not contain duplicates")
    if sizes != sorted(sizes):
        raise argparse.ArgumentTypeError("--collection-sizes must be strictly increasing")
    return sizes


def parse_tsv_line(raw: str, path: Path, line_no: int) -> Tuple[str, str]:
    raw = raw.rstrip("\n")
    if raw.endswith("\r"):
        raw = raw[:-1]
    parts = raw.split("\t")
    if len(parts) != 2:
        raise SubsetError(f"{path}:{line_no}: expected exactly two tab-separated columns")
    return parts[0], parts[1]


def check_header(handle, path: Path, expected: Tuple[str, str]) -> None:
    raw = handle.readline()
    if raw == "":
        raise SubsetError(f"{path}: empty file")
    header = parse_tsv_line(raw, path, 1)
    if header != expected:
        raise SubsetError(f"{path}: expected header {expected[0]}\\t{expected[1]}, got {header[0]}\\t{header[1]}")


def read_queries(path: Path) -> Dict[str, str]:
    queries: Dict[str, str] = {}
    with path.open("r", encoding="utf-8", newline="") as handle:
        check_header(handle, path, QUERIES_HEADER)
        for line_no, raw in enumerate(handle, start=2):
            qid, text = parse_tsv_line(raw, path, line_no)
            if not qid:
                raise SubsetError(f"{path}:{line_no}: empty qid")
            if qid in queries:
                raise SubsetError(f"{path}:{line_no}: duplicate qid {qid}")
            queries[qid] = text
    return queries


def read_collection_ids(path: Path) -> Set[str]:
    pids: Set[str] = set()
    with path.open("r", encoding="utf-8", newline="") as handle:
        check_header(handle, path, COLLECTION_HEADER)
        for line_no, raw in enumerate(handle, start=2):
            pid, _text = parse_tsv_line(raw, path, line_no)
            if not pid:
                raise SubsetError(f"{path}:{line_no}: empty pid")
            if pid in pids:
                raise SubsetError(f"{path}:{line_no}: duplicate pid {pid}")
            pids.add(pid)
    return pids


def read_valid_qrels(path: Path, queries: Dict[str, str], pids: Set[str]) -> Dict[str, Set[str]]:
    qrels: Dict[str, Set[str]] = {}
    seen_pairs: Set[Tuple[str, str]] = set()
    with path.open("r", encoding="utf-8", newline="") as handle:
        check_header(handle, path, QRELS_HEADER)
        for line_no, raw in enumerate(handle, start=2):
            qid, pid = parse_tsv_line(raw, path, line_no)
            if not qid:
                raise SubsetError(f"{path}:{line_no}: empty qid")
            if not pid:
                raise SubsetError(f"{path}:{line_no}: empty pid")
            pair = (qid, pid)
            if pair in seen_pairs:
                raise SubsetError(f"{path}:{line_no}: duplicate qid-pid pair {qid}\\t{pid}")
            seen_pairs.add(pair)
            if qid in queries and pid in pids:
                qrels.setdefault(qid, set()).add(pid)
    return qrels


def stable_hash(parts: Sequence[str]) -> str:
    return hashlib.sha256("\t".join(parts).encode("utf-8")).hexdigest()


def query_key(source_revision: str, qid: str) -> Tuple[str, str]:
    return stable_hash((source_revision, "query", qid)), qid


def document_key(source_revision: str, pid: str) -> Tuple[str, str]:
    return stable_hash((source_revision, "document", pid)), pid


def checksum_lines(values: Iterable[str]) -> str:
    h = hashlib.sha256()
    for value in values:
        h.update(value.encode("utf-8"))
        h.update(b"\n")
    return h.hexdigest()


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def output_collection_label(size: int) -> str:
    if size % 1000 == 0:
        return f"{size // 1000}k"
    return str(size)


def required_files(query_count: int, collection_sizes: Sequence[int]) -> List[str]:
    files = [
        "manifest.json",
        f"queries.dev.{query_count}.tsv",
        f"qrels.retrieval.dev.{query_count}.tsv",
    ]
    for size in collection_sizes:
        label = output_collection_label(size)
        files.append(f"collection.{label}.tsv")
        files.append(f"collection.{label}.ids")
    return files


def select_queries(
    source_revision: str,
    query_count: int,
    queries: Dict[str, str],
    valid_qrels: Dict[str, Set[str]],
) -> List[str]:
    candidates = [qid for qid in valid_qrels.keys() if qid in queries and valid_qrels[qid]]
    selected = sorted(candidates, key=lambda qid: query_key(source_revision, qid))[:query_count]
    if len(selected) < query_count:
        raise SubsetError(
            f"not enough candidate queries: requested {query_count}, found {len(selected)}"
        )
    return selected


def select_documents(
    source_revision: str,
    pids: Set[str],
    required_positive_pids: Set[str],
    collection_sizes: Sequence[int],
) -> Tuple[List[str], List[str]]:
    min_size = min(collection_sizes)
    max_size = max(collection_sizes)
    if len(required_positive_pids) > min_size:
        raise SubsetError(
            f"unique positive pid count {len(required_positive_pids)} exceeds smallest collection size {min_size}"
        )
    if len(pids) < max_size:
        raise SubsetError(f"not enough collection documents: requested {max_size}, found {len(pids)}")

    ordered_positive = sorted(required_positive_pids, key=lambda pid: document_key(source_revision, pid))
    needed_non_positive = max_size - len(ordered_positive)
    non_positive_iter = (pid for pid in pids if pid not in required_positive_pids)
    ordered_non_positive = heapq.nsmallest(
        needed_non_positive,
        non_positive_iter,
        key=lambda pid: document_key(source_revision, pid),
    )
    if len(ordered_non_positive) < needed_non_positive:
        raise SubsetError(
            f"not enough non-positive documents: requested {needed_non_positive}, found {len(ordered_non_positive)}"
        )
    return ordered_positive, ordered_positive + ordered_non_positive


def read_selected_collection_texts(path: Path, selected_pids: Set[str]) -> Dict[str, str]:
    texts: Dict[str, str] = {}
    with path.open("r", encoding="utf-8", newline="") as handle:
        check_header(handle, path, COLLECTION_HEADER)
        for line_no, raw in enumerate(handle, start=2):
            pid, text = parse_tsv_line(raw, path, line_no)
            if pid in selected_pids:
                texts[pid] = text
    missing = sorted(selected_pids - texts.keys())
    if missing:
        preview = ", ".join(missing[:5])
        raise SubsetError(f"selected collection text missing for {len(missing)} pid(s): {preview}")
    return texts


def write_tsv(path: Path, header: Tuple[str, str], rows: Iterable[Tuple[str, str]]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(f"{header[0]}\t{header[1]}\n")
        for left, right in rows:
            handle.write(f"{left}\t{right}\n")


def write_ids(path: Path, ids: Iterable[str]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for value in ids:
            handle.write(f"{value}\n")


def read_tsv_ids(path: Path, expected_header: Tuple[str, str]) -> List[str]:
    ids: List[str] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        check_header(handle, path, expected_header)
        for line_no, raw in enumerate(handle, start=2):
            left, _right = parse_tsv_line(raw, path, line_no)
            ids.append(left)
    return ids


def read_plain_ids(path: Path) -> List[str]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        ids = [line.rstrip("\n").rstrip("\r") for line in handle]
    if any(not value for value in ids):
        raise SubsetError(f"{path}: empty id in ids file")
    return ids


def build_manifest(
    tmp_dir: Path,
    args,
    selected_qids: List[str],
    selected_pairs: List[Tuple[str, str]],
    ordered_positive_pids: List[str],
    collection_doc_order: List[str],
    collection_sizes: Sequence[int],
    verification: Dict[str, object],
) -> Dict[str, object]:
    input_layout = getattr(args, "input_layout", ".")
    input_prefix = "" if input_layout == "." else f"{input_layout}/"
    files: Dict[str, Dict[str, object]] = {}
    for filename in required_files(args.query_count, collection_sizes):
        if filename == "manifest.json":
            continue
        path = tmp_dir / filename
        info: Dict[str, object] = {
            "sha256": file_sha256(path),
            "bytes": path.stat().st_size,
        }
        if filename.startswith("queries.dev.") or filename.startswith("qrels.") or filename.startswith("collection.") and filename.endswith(".tsv"):
            with path.open("r", encoding="utf-8", newline="") as handle:
                info["data_rows"] = max(sum(1 for _ in handle) - 1, 0)
        elif filename.endswith(".ids"):
            with path.open("r", encoding="utf-8", newline="") as handle:
                info["rows"] = sum(1 for _ in handle)
        files[filename] = info

    collection_counts = {
        str(size): len(collection_doc_order[:size])
        for size in collection_sizes
    }
    return {
        "schema_version": 1,
        "dataset": "T2Ranking",
        "source_revision": args.source_revision,
        "parameters": {
            "query_count": args.query_count,
            "collection_sizes": list(collection_sizes),
        },
        "source_files": {
            "input_layout": input_layout,
            "collection": f"{input_prefix}collection.tsv",
            "queries": f"{input_prefix}queries.dev.tsv",
            "qrels": f"{input_prefix}qrels.retrieval.dev.tsv",
        },
        "selection_algorithm": {
            "version": ALGORITHM_VERSION,
            "query_hash": 'SHA256(source_revision + "\\tquery\\t" + qid), tie-break qid',
            "document_hash": 'SHA256(source_revision + "\\tdocument\\t" + pid), tie-break pid',
            "document_order": "all selected positive pids by document hash, then non-positive pids by document hash",
            "qrels_order": "selected query order, then positive pids by document hash",
            "source_order_dependent": False,
        },
        "counts": {
            "selected_query_count": len(selected_qids),
            "positive_pair_count": len(selected_pairs),
            "unique_positive_pid_count": len(ordered_positive_pids),
            "collection_rows": collection_counts,
        },
        "checksums": {
            "selected_query_ids_sha256": checksum_lines(selected_qids),
            "required_positive_ids_sha256": checksum_lines(ordered_positive_pids),
        },
        "files": files,
        "verification": verification,
    }


def verify_temp_outputs(
    tmp_dir: Path,
    query_count: int,
    collection_sizes: Sequence[int],
    selected_qids: List[str],
    selected_pairs: List[Tuple[str, str]],
    ordered_positive_pids: List[str],
) -> Dict[str, object]:
    query_file = tmp_dir / f"queries.dev.{query_count}.tsv"
    qrels_file = tmp_dir / f"qrels.retrieval.dev.{query_count}.tsv"
    written_qids = read_tsv_ids(query_file, QUERIES_HEADER)
    if written_qids != selected_qids:
        raise SubsetError("queries output order does not match selected query order")

    written_pairs: List[Tuple[str, str]] = []
    with qrels_file.open("r", encoding="utf-8", newline="") as handle:
        check_header(handle, qrels_file, QRELS_HEADER)
        for line_no, raw in enumerate(handle, start=2):
            written_pairs.append(parse_tsv_line(raw, qrels_file, line_no))
    if written_pairs != selected_pairs:
        raise SubsetError("qrels output does not match selected positive pairs")

    by_qid: Dict[str, int] = {qid: 0 for qid in selected_qids}
    for qid, _pid in written_pairs:
        by_qid[qid] = by_qid.get(qid, 0) + 1
    selected_queries_have_positive = all(by_qid.get(qid, 0) > 0 for qid in selected_qids)
    if not selected_queries_have_positive:
        raise SubsetError("at least one selected query has no positive qrel")

    positive_set = set(ordered_positive_pids)
    ids_by_size: Dict[int, List[str]] = {}
    ids_match_tsv_order = True
    positives_in_all = True
    for size in collection_sizes:
        label = output_collection_label(size)
        tsv_path = tmp_dir / f"collection.{label}.tsv"
        ids_path = tmp_dir / f"collection.{label}.ids"
        tsv_ids = read_tsv_ids(tsv_path, COLLECTION_HEADER)
        plain_ids = read_plain_ids(ids_path)
        if len(tsv_ids) != size or len(plain_ids) != size:
            raise SubsetError(f"collection {size} row count mismatch")
        if tsv_ids != plain_ids:
            ids_match_tsv_order = False
        if len(set(plain_ids)) != len(plain_ids):
            raise SubsetError(f"collection {size} output contains duplicate pid")
        if not positive_set.issubset(set(plain_ids)):
            positives_in_all = False
        ids_by_size[size] = plain_ids
    if not ids_match_tsv_order:
        raise SubsetError("collection ids files do not match TSV row order")
    if not positives_in_all:
        raise SubsetError("required positive pids are not present in every collection subset")

    nested_prefix = True
    for smaller, larger in zip(collection_sizes, collection_sizes[1:]):
        if ids_by_size[smaller] != ids_by_size[larger][:smaller]:
            nested_prefix = False
            break
    if not nested_prefix:
        raise SubsetError("collection subsets are not nested by prefix")

    return {
        "selected_queries_have_positive": selected_queries_have_positive,
        "ids_match_tsv_order": ids_match_tsv_order,
        "nested_prefix": nested_prefix,
        "positive_pids_present_in_all_collections": positives_in_all,
    }


def same_tree_outputs(tmp_dir: Path, output_root: Path, filenames: Sequence[str]) -> bool:
    for filename in filenames:
        final_path = output_root / filename
        tmp_path = tmp_dir / filename
        if not final_path.is_file():
            return False
        if file_sha256(final_path) != file_sha256(tmp_path):
            return False
    return True


def publish_outputs(tmp_dir: Path, output_root: Path, filenames: Sequence[str]) -> str:
    if not output_root.exists():
        os.replace(tmp_dir, output_root)
        return "published"
    if not output_root.is_dir():
        raise SubsetError(f"output root exists and is not a directory: {output_root}")
    if same_tree_outputs(tmp_dir, output_root, filenames):
        shutil.rmtree(tmp_dir)
        return "unchanged"
    existing = list(output_root.iterdir())
    if not existing:
        output_root.rmdir()
        os.replace(tmp_dir, output_root)
        return "published"
    raise SubsetError(
        f"refusing to overwrite non-matching existing output root: {output_root}"
    )


def resolve_input_paths(dataset_root: Path) -> Tuple[Path, Path, Path, str]:
    direct = (
        dataset_root / "collection.tsv",
        dataset_root / "queries.dev.tsv",
        dataset_root / "qrels.retrieval.dev.tsv",
    )
    if all(path.is_file() for path in direct):
        return direct[0], direct[1], direct[2], "."

    data_dir = dataset_root / "data"
    nested = (
        data_dir / "collection.tsv",
        data_dir / "queries.dev.tsv",
        data_dir / "qrels.retrieval.dev.tsv",
    )
    if all(path.is_file() for path in nested):
        return nested[0], nested[1], nested[2], "data"

    missing = [str(path) for path in direct if not path.is_file()]
    nested_missing = [str(path) for path in nested if not path.is_file()]
    raise SubsetError(
        "required input files missing; checked direct root and data/ subdir: "
        + ", ".join(missing + nested_missing)
    )


def generate(args) -> Dict[str, object]:
    dataset_root = Path(args.dataset_root)
    output_root = Path(args.output_root)
    collection_path, queries_path, qrels_path, input_layout = resolve_input_paths(dataset_root)
    args.input_layout = input_layout

    queries = read_queries(queries_path)
    pids = read_collection_ids(collection_path)
    valid_qrels = read_valid_qrels(qrels_path, queries, pids)

    selected_qids = select_queries(args.source_revision, args.query_count, queries, valid_qrels)
    required_positive_pids: Set[str] = set()
    for qid in selected_qids:
        required_positive_pids.update(valid_qrels[qid])
    ordered_positive_pids, collection_doc_order = select_documents(
        args.source_revision,
        pids,
        required_positive_pids,
        args.collection_sizes,
    )
    selected_pid_set = set(collection_doc_order[: max(args.collection_sizes)])
    selected_texts = read_selected_collection_texts(collection_path, selected_pid_set)

    selected_pairs: List[Tuple[str, str]] = []
    for qid in selected_qids:
        for pid in sorted(valid_qrels[qid], key=lambda p: document_key(args.source_revision, p)):
            selected_pairs.append((qid, pid))

    parent = output_root.parent
    parent.mkdir(parents=True, exist_ok=True)
    tmp_dir = Path(tempfile.mkdtemp(prefix=f".{output_root.name}.tmp.", dir=str(parent)))
    filenames = required_files(args.query_count, args.collection_sizes)
    try:
        write_tsv(
            tmp_dir / f"queries.dev.{args.query_count}.tsv",
            QUERIES_HEADER,
            ((qid, queries[qid]) for qid in selected_qids),
        )
        write_tsv(
            tmp_dir / f"qrels.retrieval.dev.{args.query_count}.tsv",
            QRELS_HEADER,
            selected_pairs,
        )
        for size in args.collection_sizes:
            label = output_collection_label(size)
            ids = collection_doc_order[:size]
            write_tsv(
                tmp_dir / f"collection.{label}.tsv",
                COLLECTION_HEADER,
                ((pid, selected_texts[pid]) for pid in ids),
            )
            write_ids(tmp_dir / f"collection.{label}.ids", ids)

        verification = verify_temp_outputs(
            tmp_dir,
            args.query_count,
            args.collection_sizes,
            selected_qids,
            selected_pairs,
            ordered_positive_pids,
        )
        manifest = build_manifest(
            tmp_dir,
            args,
            selected_qids,
            selected_pairs,
            ordered_positive_pids,
            collection_doc_order,
            args.collection_sizes,
            verification,
        )
        manifest_path = tmp_dir / "manifest.json"
        with manifest_path.open("w", encoding="utf-8", newline="\n") as handle:
            json.dump(manifest, handle, ensure_ascii=False, indent=2, sort_keys=True)
            handle.write("\n")
        status = publish_outputs(tmp_dir, output_root, filenames)
        tmp_dir = None  # type: ignore[assignment]
    finally:
        if tmp_dir is not None:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    summary = {
        "status": status,
        "output_root": str(output_root),
        "selected_query_count": len(selected_qids),
        "positive_pair_count": len(selected_pairs),
        "unique_positive_pid_count": len(ordered_positive_pids),
        "collection_sizes": args.collection_sizes,
        "selected_query_ids_sha256": checksum_lines(selected_qids),
        "required_positive_ids_sha256": checksum_lines(ordered_positive_pids),
        "input_layout": input_layout,
    }
    return summary


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-root", required=True)
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--query-count", type=int, default=200)
    parser.add_argument("--collection-sizes", type=parse_sizes, default=parse_sizes("10000,100000"))
    args = parser.parse_args(argv)
    if args.query_count <= 0:
        parser.error("--query-count must be positive")
    if not args.source_revision:
        parser.error("--source-revision must not be empty")
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    try:
        summary = generate(args)
    except SubsetError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1
    print(json.dumps(summary, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
