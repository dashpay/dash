# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

package=platform_cxx
$(package)_version=d72b4d5159da3c98417a5249ffdd8273b5f7fc6f
$(package)_download_path=https://github.com/dashpay/platform/archive
$(package)_download_file=$($(package)_version).tar.gz
$(package)_file_name=platform-$($(package)_version).tar.gz
$(package)_sha256_hash=5733e9d53765b2e00490409442b96d984341439e05e15ca62429a0ceee1e3bc1
$(package)_dependencies=native_rust rust_stdlib native_protobuf tenderdash_sources
$(package)_patches=cargo-config.toml rustc-linker.sh
$(package)_vendored_file_name=platform-cxx-$($(package)_version)-vendored.tar.gz
$(package)_cargo_manifest=Cargo.toml
$(package)_cargo_lock_path=Cargo.lock

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
  $($(package)_cargo) build --locked --offline --release --target $(rust_stdlib_target) -p dash-platform-cxx
endef

# The crate's build.rs stages the generated bridge header, the cxx runtime
# header and signer.h under target/<triple>/release/include; that tree plus
# the static archive is the whole installed interface.
define $(package)_stage_cmds
  mkdir -p $($(package)_staging_prefix_dir)/include $($(package)_staging_prefix_dir)/lib && \
  cp -R target/$(rust_stdlib_target)/release/include/. $($(package)_staging_prefix_dir)/include/ && \
  cp target/$(rust_stdlib_target)/release/libdash_platform_cxx.a $($(package)_staging_prefix_dir)/lib/
endef
