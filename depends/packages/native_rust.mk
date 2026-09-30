# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# To update the Rust compiler, change the version below and then run the script
# ./contrib/devtools/update-rust-hashes.py

package:=native_rust
$(package)_version:=1.98.1
$(package)_download_path:=https://static.rust-lang.org/dist
$(package)_patches:=fix-elf-interpreter.sh

# Linux (ARMv8)
$(package)_build_target_aarch64_linux:=aarch64-unknown-linux-gnu
$(package)_file_name_aarch64_linux:=rust-$($(package)_version)-$($(package)_build_target_aarch64_linux).tar.gz
$(package)_sha256_hash_aarch64_linux:=f00ba576645cef658e1deed96fab8f707958e9d58808b16343448b5d1c4f7407

# Linux (x86_64)
$(package)_build_target_x86_64_linux:=x86_64-unknown-linux-gnu
$(package)_file_name_x86_64_linux:=rust-$($(package)_version)-$($(package)_build_target_x86_64_linux).tar.gz
$(package)_sha256_hash_x86_64_linux:=24ba1338a2d35c5a3247936546429e163fa674d726102af18bdf624582c57aea

# macOS (ARMv8)
$(package)_build_target_aarch64_darwin:=aarch64-apple-darwin
$(package)_file_name_aarch64_darwin:=rust-$($(package)_version)-$($(package)_build_target_aarch64_darwin).tar.gz
$(package)_sha256_hash_aarch64_darwin:=cfc171d8120d401b10a1028c52646dd8e00e3e66852f949061ce087845f55afd

# macOS (x86_64)
$(package)_build_target_x86_64_darwin:=x86_64-apple-darwin
$(package)_file_name_x86_64_darwin:=rust-$($(package)_version)-$($(package)_build_target_x86_64_darwin).tar.gz
$(package)_sha256_hash_x86_64_darwin:=443a1165abbac41c9143b83ff837c0fb1d8c03d2f8fb1da27427bc9fc646aad3

# The Rust target triple of the build machine.
$(package)_build_target:=$($(package)_build_target_$(build_arch)_$(build_os))
$(package)_file_name=$($(package)_file_name_$(build_arch)_$(build_os))
$(package)_sha256_hash=$($(package)_sha256_hash_$(build_arch)_$(build_os))

ifeq ($($(package)_file_name),)
$(error native_rust has no prebuilt Rust $($(package)_version) for $(build_arch)-$(build_os))
endif

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_prefix_dir)/bin $($(package)_staging_prefix_dir)/lib/rustlib && \
  cp cargo/bin/cargo rustc/bin/rustc $($(package)_staging_prefix_dir)/bin/ && \
  cp -R rustc/lib/. $($(package)_staging_prefix_dir)/lib/ && \
  cp -R rust-std-*/lib/rustlib/. $($(package)_staging_prefix_dir)/lib/rustlib/ && \
  bash $($(package)_patch_dir)/fix-elf-interpreter.sh \
    $($(package)_staging_prefix_dir)/lib \
    $($(package)_staging_prefix_dir)/bin/cargo \
    $($(package)_staging_prefix_dir)/bin/rustc
endef
