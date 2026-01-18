# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# To update the Rust stdlib, change the version below and then run the script
# ./contrib/devtools/update-rust-hashes.py

package:=rust_stdlib
$(package)_version:=1.82.0
$(package)_download_path:=https://static.rust-lang.org/dist
$(package)_dependencies:=native_rust
$(package)_target=$(or \
  $($(package)_target_$(canonical_host)),\
  $($(package)_target_$(subst -pc-,-unknown-,$(canonical_host))),\
  $($(package)_target_$(subst -unknown-,-pc-,$(canonical_host))),\
  $($(package)_target_$(subst -linux-,-unknown-linux-,$(canonical_host))))

# FreeBSD (x86_64)
$(package)_targets += x86_64-unknown-freebsd
$(package)_target_x86_64-unknown-freebsd:=x86_64-unknown-freebsd
$(package)_sha256_hash_x86_64-unknown-freebsd:=be1acaf3c2f15d42b05b1f032db5ac3b11a0ac5a91c0efef26f2d8135d68a829

# Linux (ARMv7)
$(package)_targets += armv7-unknown-linux-gnueabihf
$(package)_target_arm-unknown-linux-gnueabihf:=armv7-unknown-linux-gnueabihf
$(package)_target_armv7-unknown-linux-gnueabihf:=armv7-unknown-linux-gnueabihf
$(package)_sha256_hash_armv7-unknown-linux-gnueabihf:=5dd8b36467e03ba47bfa7ea5d7578c66bccb648dd2129d7cec6fb3ff00f81ca3

# Linux (ARMv8)
$(package)_targets += aarch64-unknown-linux-gnu
$(package)_target_aarch64-unknown-linux-gnu:=aarch64-unknown-linux-gnu
$(package)_sha256_hash_aarch64-unknown-linux-gnu:=82b2308ee531775bf4d1faa57bddfae85f363bec43ca36ba6db4ebad7c1450d4

# Linux (PowerPC 64-bit little-endian)
$(package)_targets += powerpc64le-unknown-linux-gnu
$(package)_target_powerpc64le-unknown-linux-gnu:=powerpc64le-unknown-linux-gnu
$(package)_sha256_hash_powerpc64le-unknown-linux-gnu:=142c7c2896fa4596b5c4c35d9d5e4d80acd5a699e5fa0560d92a89eda035ece3

# Linux (RISCV64GC)
$(package)_targets += riscv64gc-unknown-linux-gnu
$(package)_target_riscv64-unknown-linux-gnu:=riscv64gc-unknown-linux-gnu
$(package)_target_riscv64gc-unknown-linux-gnu:=riscv64gc-unknown-linux-gnu
$(package)_sha256_hash_riscv64gc-unknown-linux-gnu:=7b35c8207c77e3fc2f7f7a26dea989cc2cdc13a955851ff74d4882f96f4e14dd

# Linux (x86_64)
$(package)_targets += x86_64-unknown-linux-gnu
$(package)_target_x86_64-unknown-linux-gnu:=x86_64-unknown-linux-gnu
$(package)_sha256_hash_x86_64-unknown-linux-gnu:=e7e808b8745298369fa3bbc3c0b7af9ca0fb995661bd684a7022d14bc9ae0057

# macOS (ARMv8)
$(package)_targets += aarch64-apple-darwin
$(package)_target_aarch64-apple-darwin:=aarch64-apple-darwin
$(package)_target_arm64-apple-darwin:=aarch64-apple-darwin
$(package)_sha256_hash_aarch64-apple-darwin:=5ec28e75ed8715efaa2490d76ae026a34b13df6899d98b14d0a6995556f4e6b4

# macOS (x86_64)
$(package)_targets += x86_64-apple-darwin
$(package)_target_x86_64-apple-darwin:=x86_64-apple-darwin
$(package)_sha256_hash_x86_64-apple-darwin:=52084c8cdb34ca139a00f9f03f1a582d96b677e9f223a8d1aa31ae575a06cc16

# Windows (x86_64)
$(package)_targets += x86_64-pc-windows-gnu
$(package)_target_x86_64-w64-mingw32:=x86_64-pc-windows-gnu
$(package)_sha256_hash_x86_64-pc-windows-gnu:=32d42270b114c9341e5bc9b1d24f336024889ddd32a7d22e4700cc3c45fe9d3d

$(package)_file_name=rust-std-$($(package)_version)-$($(package)_target).tar.gz
$(package)_sha256_hash=$($(package)_sha256_hash_$($(package)_target))

define $(package)_fetch_cmds
  $(call fetch_file,$(package),$($(package)_download_path),$($(package)_file_name),$($(package)_file_name),$($(package)_sha256_hash))
endef

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib && \
  cp -r rust-std-$($(package)_target)/lib/rustlib/$($(package)_target) $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib/
endef
