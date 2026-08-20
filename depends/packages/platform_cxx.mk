# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

package=platform_cxx
$(package)_version=df4fdb68559ef57d50624b7f0841594aef8647e5
$(package)_download_path=https://github.com/dashpay/platform/archive
$(package)_download_file=$($(package)_version).tar.gz
$(package)_file_name=platform-$($(package)_version).tar.gz
$(package)_sha256_hash=935b64a4f3acf48840706d573acc4c43ce4ac44272265ea96af45e35c47d829a
$(package)_build_subdir=packages/rs-platform-cxx/standalone
$(package)_dependencies=native_rust rust_stdlib native_protobuf tenderdash_sources
$(package)_patches=cargo-config.toml
$(package)_vendored_file_name=platform-cxx-$($(package)_version)-vendored.tar.gz
$(package)_cargo_manifest=packages/rs-platform-cxx/standalone/Cargo.toml
$(package)_cargo_lock_path=packages/rs-platform-cxx/standalone/Cargo.lock

define $(package)_preprocess_cmds
  true
endef

define $(package)_build_cmds
  mkdir -p target && \
  cp $(host_prefix)/tenderdash-sources/tenderdash-*.zip target/ && \
  CARGO_BUILD_TARGET=$(rust_stdlib_target) \
  CARGO_TARGET_DIR=$($(package)_build_dir)/target \
  PROTOC=$(build_prefix)/bin/protoc \
  PROTOC_INCLUDE=$(build_prefix)/include \
  $($(package)_cargo) build --locked --offline --release --target $(rust_stdlib_target)
endef

define $(package)_stage_cmds
  CARGO_BUILD_TARGET=$(rust_stdlib_target) \
  CARGO_PROFILE=release \
  CARGO_TARGET_DIR=$($(package)_build_dir)/target \
  bash ../install.sh $($(package)_staging_prefix_dir)
endef
