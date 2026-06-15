#!/usr/bin/env bash
set -e

ONNXRUNTIME_VERSION="${ONNXRUNTIME_VERSION:-1.24.4}"
ONNXRUNTIME_DIR="${ONNXRUNTIME_DIR:-third_party/onnxruntime}"
MODEL_DIR="${MODEL_DIR:-models/bge-small-zh-v1.5}"
ONNX_REPO="${ONNX_REPO:-onnx-community/bge-small-zh-v1.5-ONNX}"
VOCAB_REPO="${VOCAB_REPO:-BAAI/bge-small-zh-v1.5}"

ORT_ARCHIVE="onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz"
ORT_URL="https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/${ORT_ARCHIVE}"

die() {
	echo "error: $*" >&2
	exit 1
}

info() {
	echo "[setup-onnx] $*"
}

check_project_root() {
	if [ ! -f Makefile ] || [ ! -d src ] || [ ! -d include ]; then
		die "please run this script from the KVLite project root"
	fi
}

check_platform() {
	local os arch
	os="$(uname -s)"
	arch="$(uname -m)"
	if [ "$os" != "Linux" ] || [ "$arch" != "x86_64" ]; then
		die "only Linux x86_64 is supported in this first ONNX setup script (got ${os} ${arch})"
	fi
}

have_onnxruntime() {
	[ -f "${ONNXRUNTIME_DIR}/include/onnxruntime_c_api.h" ] &&
	[ -f "${ONNXRUNTIME_DIR}/lib/libonnxruntime.so" ]
}

download_file() {
	local url="$1"
	local out="$2"

	if command -v curl >/dev/null 2>&1; then
		curl -L --fail --retry 2 -o "$out" "$url"
	elif command -v wget >/dev/null 2>&1; then
		wget -O "$out" "$url"
	else
		die "curl or wget is required to download ONNX Runtime"
	fi
}

setup_onnxruntime() {
	local tmpdir archive extracted

	if have_onnxruntime; then
		info "ONNX Runtime already exists: ${ONNXRUNTIME_DIR}"
		return
	fi

	info "downloading ONNX Runtime ${ONNXRUNTIME_VERSION}"
	tmpdir="$(mktemp -d)"
	archive="${tmpdir}/${ORT_ARCHIVE}"
	download_file "$ORT_URL" "$archive" || die "failed to download ${ORT_URL}"

	tar -xzf "$archive" -C "$tmpdir"
	extracted="${tmpdir}/onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}"
	[ -f "${extracted}/include/onnxruntime_c_api.h" ] ||
		die "downloaded ONNX Runtime archive does not contain onnxruntime_c_api.h"
	[ -f "${extracted}/lib/libonnxruntime.so" ] ||
		die "downloaded ONNX Runtime archive does not contain libonnxruntime.so"

	mkdir -p third_party
	rm -rf "$ONNXRUNTIME_DIR"
	mv "$extracted" "$ONNXRUNTIME_DIR"
	rm -rf "$tmpdir"
	info "ONNX Runtime installed to ${ONNXRUNTIME_DIR}"
}

find_hf_cli() {
	if command -v hf >/dev/null 2>&1; then
		echo "hf"
		return
	fi

	if command -v huggingface-cli >/dev/null 2>&1; then
		echo "huggingface-cli"
		return
	fi

	return 1
}

hf_download() {
	local hf_cmd="$1"
	local repo="$2"
	local file="$3"
	local target_dir="$4"

	if "$hf_cmd" download --help 2>&1 | grep -q -- "--local-dir-use-symlinks"; then
		"$hf_cmd" download "$repo" "$file" --local-dir "$target_dir" --local-dir-use-symlinks False
	else
		"$hf_cmd" download "$repo" "$file" --local-dir "$target_dir"
	fi
}

copy_onnx_to_root() {
	if [ ! -f "${MODEL_DIR}/model.onnx" ] && [ -f "${MODEL_DIR}/onnx/model.onnx" ]; then
		cp -f "${MODEL_DIR}/onnx/model.onnx" "${MODEL_DIR}/model.onnx"
	fi

	if [ ! -f "${MODEL_DIR}/model.onnx_data" ] && [ -f "${MODEL_DIR}/onnx/model.onnx_data" ]; then
		cp -f "${MODEL_DIR}/onnx/model.onnx_data" "${MODEL_DIR}/model.onnx_data"
	fi
}

have_model_files() {
	[ -f "${MODEL_DIR}/model.onnx" ] &&
	[ -f "${MODEL_DIR}/model.onnx_data" ] &&
	[ -f "${MODEL_DIR}/vocab.txt" ] &&
	[ -f "${MODEL_DIR}/tokenizer.json" ] &&
	[ -f "${MODEL_DIR}/tokenizer_config.json" ] &&
	[ -f "${MODEL_DIR}/config.json" ] &&
	[ -f "${MODEL_DIR}/special_tokens_map.json" ]
}

setup_model() {
	local hf_cmd

	mkdir -p "$MODEL_DIR"
	copy_onnx_to_root

	if have_model_files; then
		info "bge-small-zh-v1.5 model files already exist: ${MODEL_DIR}"
		return
	fi

	hf_cmd="$(find_hf_cli)" || die "Hugging Face CLI is required for model download. Install it with: python3 -m pip install -U huggingface_hub"

	info "downloading ONNX model files from ${ONNX_REPO}"
	hf_download "$hf_cmd" "$ONNX_REPO" "tokenizer.json" "$MODEL_DIR"
	hf_download "$hf_cmd" "$ONNX_REPO" "tokenizer_config.json" "$MODEL_DIR"
	hf_download "$hf_cmd" "$ONNX_REPO" "onnx/model.onnx" "$MODEL_DIR"
	hf_download "$hf_cmd" "$ONNX_REPO" "onnx/model.onnx_data" "$MODEL_DIR"
	copy_onnx_to_root

	info "downloading vocab/config files from ${VOCAB_REPO}"
	hf_download "$hf_cmd" "$VOCAB_REPO" "vocab.txt" "$MODEL_DIR"
	hf_download "$hf_cmd" "$VOCAB_REPO" "config.json" "$MODEL_DIR"
	hf_download "$hf_cmd" "$VOCAB_REPO" "special_tokens_map.json" "$MODEL_DIR"

	have_model_files || die "model setup incomplete; please inspect ${MODEL_DIR}"
	info "model files installed to ${MODEL_DIR}"
}

print_summary() {
	info "environment ready"
	echo ""
	echo "Required files:"
	echo "  ${ONNXRUNTIME_DIR}/include/onnxruntime_c_api.h"
	echo "  ${ONNXRUNTIME_DIR}/lib/libonnxruntime.so"
	echo "  ${MODEL_DIR}/model.onnx"
	echo "  ${MODEL_DIR}/model.onnx_data"
	echo "  ${MODEL_DIR}/vocab.txt"
	echo ""
echo "Next build command:"
	echo "  make embedding"
}

check_project_root
check_platform
mkdir -p third_party "$MODEL_DIR"
setup_onnxruntime
setup_model
print_summary
