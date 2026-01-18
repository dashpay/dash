# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# To update the Rust compiler, change the version below and then run the script
# ./contrib/devtools/update-rust-hashes.py

package:=native_rust
$(package)_version:=1.85.1
$(package)_download_path:=https://static.rust-lang.org/dist

# FreeBSD (x86_64)
$(package)_file_name_x86_64_freebsd:=rust-$($(package)_version)-x86_64-unknown-freebsd.tar.gz
$(package)_sha256_hash_x86_64_freebsd:=f905730e22a9a8a2dfce1ab0c50d427b7978c5b235c33018b09552041b6f6329

# Linux (ARMv8)
$(package)_file_name_aarch64_linux:=rust-$($(package)_version)-aarch64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_aarch64_linux:=d2609d8cd965060f0b4a8c509131066369e8d3d31a92fedce177b42b32af6b4d

# Linux (x86_64)
$(package)_file_name_x86_64_linux:=rust-$($(package)_version)-x86_64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_x86_64_linux:=b7202563a52b47f575b284a5a4794fafd688e39bfe8fd855b5e80129e671cb7f

# macOS (ARMv8)
$(package)_file_name_aarch64_darwin:=rust-$($(package)_version)-aarch64-apple-darwin.tar.gz
$(package)_sha256_hash_aarch64_darwin:=64b0341a47e684d648c9b7defd0b7ff9d5397a64718cf803c1e114544f94bbe9

# macOS (x86_64)
$(package)_file_name_x86_64_darwin:=rust-$($(package)_version)-x86_64-apple-darwin.tar.gz
$(package)_sha256_hash_x86_64_darwin:=6e321957b7301d48e5ecf61bdeea6560400a5948b3e72830348367a8a9696ad7

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
  bash $(BASEDIR)/patches/native_rust/fix-elf-interpreter.sh \
    $($(package)_staging_dir)/$(host_prefix)/native/lib \
    $($(package)_staging_dir)/$(host_prefix)/native/bin/cargo \
    $($(package)_staging_dir)/$(host_prefix)/native/bin/rustc \
    $($(package)_staging_dir)/$(host_prefix)/native/bin/rustdoc
endef
