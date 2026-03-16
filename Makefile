SHELL := /bin/bash

# Detect architecture
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),arm64)
  CEF_ARCH := macosarm64
  PROJ_ARCH := arm64
else
  CEF_ARCH := macosx64
  PROJ_ARCH := x86_64
endif

# Homebrew prefix
BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
LLVM_PREFIX := $(BREW_PREFIX)/opt/llvm

# CEF configuration
CEF_VERSION := 144.0.11
CEF_FULL_VERSION := 144.0.11+ge135be2+chromium-144.0.7559.97
CEF_URL_VERSION := $(subst +,%2B,$(CEF_FULL_VERSION))
CEF_DOWNLOAD_URL := https://cef-builds.spotifycdn.com/cef_binary_$(CEF_URL_VERSION)_$(CEF_ARCH)_minimal.tar.bz2
CEF_DIR := $(CURDIR)/vendor/cef
CEF_TARBALL := $(CEF_DIR)/cef-minimal.tar.bz2
CEF_SRC := $(CEF_DIR)/src
CEF_BUILD := $(CEF_DIR)/build
CEF_ROOT := $(CEF_DIR)/install

# Environment for Rust builds that need bindgen/clang
export LIBCLANG_PATH := $(LLVM_PREFIX)/lib

# Output directories
BUILD_DIR := $(CURDIR)/build

.PHONY: all clean check-deps help
.PHONY: runtimes hello-runtime ddcore-runtime
.PHONY: winit webview cef servo
.PHONY: cef-deps cef-wrapper

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'

all: runtimes winit webview ## Build runtimes + winit + webview backends

check-deps: ## Check that required build tools are installed
	@echo "Checking build dependencies..."
	@command -v cargo >/dev/null || (echo "ERROR: cargo not found. Install Rust: https://rustup.rs" && exit 1)
	@command -v cmake >/dev/null || (echo "ERROR: cmake not found. Run: brew install cmake" && exit 1)
	@command -v ninja >/dev/null || (echo "ERROR: ninja not found. Run: brew install ninja" && exit 1)
	@test -d "$(LLVM_PREFIX)" || (echo "ERROR: llvm not found. Run: brew install llvm" && exit 1)
	@echo "All base dependencies OK."

# ─── Runtimes ────────────────────────────────────────────────────────

runtimes: hello-runtime ddcore-runtime ## Build all example runtimes

hello-runtime: check-deps ## Build hello_runtime.dylib
	cargo build --release -p hello_runtime

ddcore-runtime: check-deps ## Build ddcore_runtime.dylib
	cargo build --release -p ddcore_runtime

# ─── Winit Backend ───────────────────────────────────────────────────

winit: check-deps ## Build the winit backend
	cd winit && cargo build --release

# ─── WebView Backend ─────────────────────────────────────────────────

webview: check-deps ## Build the webview backend (Cocoa + WebKit)
	cmake -G Ninja -B webview/build -S webview -DCMAKE_BUILD_TYPE=Release
	ninja -C webview/build
	@echo "Built: webview/build/wef_webview.app"

# ─── CEF Backend ─────────────────────────────────────────────────────

$(CEF_TARBALL):
	@echo "Downloading CEF $(CEF_VERSION) for $(CEF_ARCH)..."
	@mkdir -p $(CEF_DIR)
	curl -L -o $(CEF_TARBALL) "$(CEF_DOWNLOAD_URL)"

$(CEF_SRC)/CMakeLists.txt: $(CEF_TARBALL)
	@echo "Extracting CEF..."
	@mkdir -p $(CEF_SRC)
	tar xf $(CEF_TARBALL) --strip-components=1 -C $(CEF_SRC)

$(CEF_ROOT)/libcef_dll_wrapper/libcef_dll_wrapper.a: $(CEF_SRC)/CMakeLists.txt
	@echo "Building CEF dll_wrapper..."
	cmake -G Ninja -B $(CEF_BUILD) -S $(CEF_SRC) \
		-DCMAKE_BUILD_TYPE=Release \
		-DPROJECT_ARCH=$(PROJ_ARCH) \
		-DUSE_SANDBOX=OFF
	ninja -C $(CEF_BUILD) libcef_dll_wrapper
	@echo "Installing CEF..."
	@mkdir -p $(CEF_ROOT)/include $(CEF_ROOT)/cmake $(CEF_ROOT)/Release $(CEF_ROOT)/libcef_dll_wrapper
	cp -r $(CEF_SRC)/include/* $(CEF_ROOT)/include/
	cp -r $(CEF_SRC)/cmake/* $(CEF_ROOT)/cmake/
	cp -r $(CEF_SRC)/Release/* $(CEF_ROOT)/Release/
	cp $(CEF_BUILD)/libcef_dll_wrapper/libcef_dll_wrapper.a $(CEF_ROOT)/libcef_dll_wrapper/

cef-deps: $(CEF_ROOT)/libcef_dll_wrapper/libcef_dll_wrapper.a ## Download and build CEF dependencies

cef: check-deps cef-deps ## Build the CEF backend
	cmake -G Ninja -B cef/build -S cef \
		-DCMAKE_BUILD_TYPE=Release \
		-DCEF_ROOT=$(CEF_ROOT)
	ninja -C cef/build
	@echo "Built: cef/build/Release/wef.app"

# ─── Clean ───────────────────────────────────────────────────────────

clean: ## Remove all build artifacts
	cargo clean
	rm -rf webview/build cef/build
	cd winit && cargo clean || true
	cd servo && cargo clean || true

clean-cef-vendor: ## Remove downloaded CEF files
	rm -rf vendor/cef
