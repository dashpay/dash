# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# To update the Rust stdlib, change the version below and then run the script
# ./contrib/devtools/update-rust-hashes.py

package:=rust_stdlib
$(package)_version:=1.85.1
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
$(package)_sha256_hash_x86_64-unknown-freebsd:=08a691bcdb5bde37178368e9e49dbd822d9e39c68b9371191bd16ab7f8b321c4

# Linux (ARMv7)
$(package)_targets += armv7-unknown-linux-musleabihf
$(package)_target_arm-unknown-linux-gnueabihf:=armv7-unknown-linux-musleabihf
$(package)_target_armv7-unknown-linux-gnueabihf:=armv7-unknown-linux-musleabihf
$(package)_sha256_hash_armv7-unknown-linux-musleabihf:=fbdb48968dd7af3a862c29e4e3ff85bcf333d97d21d262f347106542cc08b96d

# Linux (ARMv8)
$(package)_targets += aarch64-unknown-linux-musl
$(package)_target_aarch64-unknown-linux-gnu:=aarch64-unknown-linux-musl
$(package)_sha256_hash_aarch64-unknown-linux-musl:=991cc2f78d3db8fa1131ee2bb5807497e93e1efb9f447e2a7def0c4032ba4c54

# Linux (PowerPC 64-bit little-endian)
$(package)_targets += powerpc64le-unknown-linux-musl
$(package)_target_powerpc64le-unknown-linux-gnu:=powerpc64le-unknown-linux-musl
$(package)_sha256_hash_powerpc64le-unknown-linux-musl:=f6fad3f1c69acdd832ea2f487863f6428ba6e77b16c18e8db7fcd91b88e9e254

# Linux (RISCV64GC)
$(package)_targets += riscv64gc-unknown-linux-musl
$(package)_target_riscv64-unknown-linux-gnu:=riscv64gc-unknown-linux-musl
$(package)_target_riscv64gc-unknown-linux-gnu:=riscv64gc-unknown-linux-musl
$(package)_sha256_hash_riscv64gc-unknown-linux-musl:=4a85e0c909d6a3202919638c3b95a496acdfec7e1245be1c406b7c1d26c32fba

# Linux (x86_64)
$(package)_targets += x86_64-unknown-linux-musl
$(package)_target_x86_64-unknown-linux-gnu:=x86_64-unknown-linux-musl
$(package)_sha256_hash_x86_64-unknown-linux-musl:=3035f0c3ea9ae10ba1c21871c7a53cdb54a398616febffd42825965627a77216

# macOS (ARMv8)
$(package)_targets += aarch64-apple-darwin
$(package)_target_aarch64-apple-darwin:=aarch64-apple-darwin
$(package)_target_arm64-apple-darwin:=aarch64-apple-darwin
$(package)_sha256_hash_aarch64-apple-darwin:=5d2fd6b5c3c482750074b6ab04443b1ec41ca824fddc814aab6a1fbcf5cfb53a

# macOS (x86_64)
$(package)_targets += x86_64-apple-darwin
$(package)_target_x86_64-apple-darwin:=x86_64-apple-darwin
$(package)_sha256_hash_x86_64-apple-darwin:=b5111b105cfeb2772d92ca54e6f1c01d11def9c675c633f7d1ebdd09b83b0139

# Windows (x86_64)
$(package)_targets += x86_64-pc-windows-gnu
$(package)_target_x86_64-w64-mingw32:=x86_64-pc-windows-gnu
$(package)_sha256_hash_x86_64-pc-windows-gnu:=ae5c8942b3ccab5841c9ea65d1ac839c62553a763512799eb4c89de2ffad3d3e

$(package)_file_name=rust-std-$($(package)_version)-$($(package)_target).tar.gz
$(package)_sha256_hash=$($(package)_sha256_hash_$($(package)_target))

define $(package)_fetch_cmds
  $(call fetch_file,$(package),$($(package)_download_path),$($(package)_file_name),$($(package)_file_name),$($(package)_sha256_hash))
endef

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib && \
  cp -r rust-std-$($(package)_target)/lib/rustlib/$($(package)_target) $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib/
endef
