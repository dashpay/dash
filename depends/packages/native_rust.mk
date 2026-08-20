# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# To update the Rust compiler, change the version below and then run the script
# ./contrib/devtools/update-rust-hashes.py

package:=native_rust
$(package)_version:=1.92.0
$(package)_download_path:=https://static.rust-lang.org/dist
$(package)_patches:=fix-elf-interpreter.sh

# Linux (ARMv8)
$(package)_file_name_aarch64_linux:=rust-$($(package)_version)-aarch64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_aarch64_linux:=c812028423c3d7dd7ba99f66101e9e1aa3f66eab44a1285f41c363825d49dca4

# Linux (x86_64)
$(package)_file_name_x86_64_linux:=rust-$($(package)_version)-x86_64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_x86_64_linux:=6e5efd6c25953b2732d4e6b1842512536650c68cf72a8b99a0fc566012dd6ca5

# macOS (ARMv8)
$(package)_file_name_aarch64_darwin:=rust-$($(package)_version)-aarch64-apple-darwin.tar.gz
$(package)_sha256_hash_aarch64_darwin:=235a6cca2dd4881130a9ae61ad1149bbf28bba184dd4621700f0c98c97457716

# macOS (x86_64)
$(package)_file_name_x86_64_darwin:=rust-$($(package)_version)-x86_64-apple-darwin.tar.gz
$(package)_sha256_hash_x86_64_darwin:=fc6868991e61e9262272effbb8956b23428430f5f4300c1b48eaae3969f8af2a

$(package)_file_name=$($(package)_file_name_$(build_arch)_$(build_os))
$(package)_sha256_hash=$($(package)_sha256_hash_$(build_arch)_$(build_os))

define $(package)_set_vars
$(package)_stage_opts=--disable-ldconfig
$(package)_stage_build_opts=--without=rust-docs-json-preview,rust-docs
endef

define $(package)_fetch_cmds
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_file_name),$($(package)_file_name),$($(package)_sha256_hash))
endef

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_dir)/$(host_prefix)/native/bin && \
  mkdir -p $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib && \
  cp cargo/bin/cargo $($(package)_staging_dir)/$(host_prefix)/native/bin/ && \
  cp rustc/bin/rustc $($(package)_staging_dir)/$(host_prefix)/native/bin/ && \
  cp rustc/bin/rustdoc $($(package)_staging_dir)/$(host_prefix)/native/bin/ && \
  cp -r rustc/lib/* $($(package)_staging_dir)/$(host_prefix)/native/lib/ && \
  cp -r rust-std-*/lib/rustlib/* $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib/ && \
  bash $($(package)_patch_dir)/fix-elf-interpreter.sh \
    $($(package)_staging_dir)/$(host_prefix)/native/lib \
    $($(package)_staging_dir)/$(host_prefix)/native/bin/cargo \
    $($(package)_staging_dir)/$(host_prefix)/native/bin/rustc \
    $($(package)_staging_dir)/$(host_prefix)/native/bin/rustdoc
endef
