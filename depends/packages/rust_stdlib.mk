# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# This version is synchronized from native_rust.mk by update-rust-hashes.py.

package:=rust_stdlib
$(package)_version:=1.92.0
$(package)_download_path:=https://static.rust-lang.org/dist
$(package)_dependencies:=native_rust

# Rust support is deliberately confined to the hosts we actually validate
# (the Guix release set plus native development hosts). RUST=1 on any other
# host fails explicitly below rather than fetching a stdlib we never test.

# Linux (ARMv8)
$(package)_targets += aarch64-unknown-linux-musl
$(package)_target_aarch64-unknown-linux-gnu:=aarch64-unknown-linux-musl
$(package)_sha256_hash_aarch64-unknown-linux-musl:=715fbcfd8712c723947a020d0371c8a1a21f7531f2b696aeaed50ac23ba675c9

# Linux (RISCV64GC)
$(package)_targets += riscv64gc-unknown-linux-musl
$(package)_target_riscv64-unknown-linux-gnu:=riscv64gc-unknown-linux-musl
$(package)_target_riscv64gc-unknown-linux-gnu:=riscv64gc-unknown-linux-musl
$(package)_sha256_hash_riscv64gc-unknown-linux-musl:=34f5722ff2a0940bcd7ff6603a7748d2b963de72f6f713579c39c74ead06a7a0

# Linux (x86_64)
$(package)_targets += x86_64-unknown-linux-musl
$(package)_target_x86_64-unknown-linux-gnu:=x86_64-unknown-linux-musl
$(package)_sha256_hash_x86_64-unknown-linux-musl:=8bfd9a42c8295949d556587201acdb35d2bfb8b7ce55223845f337aa5614f9a3

# macOS (ARMv8)
$(package)_targets += aarch64-apple-darwin
$(package)_target_aarch64-apple-darwin:=aarch64-apple-darwin
$(package)_target_arm64-apple-darwin:=aarch64-apple-darwin
$(package)_sha256_hash_aarch64-apple-darwin:=b1f55aac4bc982ea67b68b262b711263005e470d31cab5d09d534bc1866d455a

# macOS (x86_64)
$(package)_targets += x86_64-apple-darwin
$(package)_target_x86_64-apple-darwin:=x86_64-apple-darwin
$(package)_sha256_hash_x86_64-apple-darwin:=1e5a8fee4e038ea2d35d82a680e2b9bf44ffccb3746aaf9dbdc56cb14152dcb8

# Windows (x86_64)
$(package)_targets += x86_64-pc-windows-gnu
$(package)_target_x86_64-w64-mingw32:=x86_64-pc-windows-gnu
$(package)_sha256_hash_x86_64-pc-windows-gnu:=6256f3497e3b14b6650511e84fdfb51fc632db1908ae5a173dffcdc96c80b7ce

$(package)_target:=$(or \
  $($(package)_target_$(canonical_host)),\
  $($(package)_target_$(subst -pc-,-unknown-,$(canonical_host))),\
  $($(package)_target_$(subst -unknown-,-pc-,$(canonical_host))),\
  $($(package)_target_$(subst -linux-,-unknown-linux-,$(canonical_host))),\
  $(if $(findstring -apple-darwin,$(canonical_host)),$(host_arch)-apple-darwin))

ifeq ($($(package)_target),)
$(error Unsupported Rust standard library target: $(canonical_host))
endif

$(package)_file_name=rust-std-$($(package)_version)-$($(package)_target).tar.gz
$(package)_sha256_hash=$($(package)_sha256_hash_$($(package)_target))

define $(package)_fetch_cmds
  $(call fetch_file,$(package),$($(package)_download_path),$($(package)_file_name),$($(package)_file_name),$($(package)_sha256_hash))
endef

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib && \
  cp -r rust-std-$($(package)_target)/lib/rustlib/$($(package)_target) $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib/
endef
