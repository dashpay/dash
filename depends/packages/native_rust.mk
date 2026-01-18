# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# To update the Rust compiler, change the version below and then run the script
# ./contrib/devtools/update-rust-hashes.py

package:=native_rust
$(package)_version:=1.82.0
$(package)_download_path:=https://static.rust-lang.org/dist

# FreeBSD (x86_64)
$(package)_file_name_x86_64_freebsd:=rust-$($(package)_version)-x86_64-unknown-freebsd.tar.gz
$(package)_sha256_hash_x86_64_freebsd:=f7b51943dbed0af3387e3269c1767fee916fb22b8e7897b3594bf5e422403137

# Linux (ARMv8)
$(package)_file_name_aarch64_linux:=rust-$($(package)_version)-aarch64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_aarch64_linux:=d7db04fce65b5f73282941f3f1df5893be9810af17eb7c65b2e614461fe31a48

# Linux (x86_64)
$(package)_file_name_x86_64_linux:=rust-$($(package)_version)-x86_64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_x86_64_linux:=0265c08ae997c4de965048a244605fb1f24a600bbe35047b811c638b8fcf676b

# macOS (ARMv8)
$(package)_file_name_aarch64_darwin:=rust-$($(package)_version)-aarch64-apple-darwin.tar.gz
$(package)_sha256_hash_aarch64_darwin:=49b6d36b308addcfd21ae56c94957688338ba7b8985bff57fc626c8e1b32f62c

# macOS (x86_64)
$(package)_file_name_x86_64_darwin:=rust-$($(package)_version)-x86_64-apple-darwin.tar.gz
$(package)_sha256_hash_x86_64_darwin:=b1a289cabc523f259f65116a41374ac159d72fbbf6c373bd5e545c8e835ceb6a

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
