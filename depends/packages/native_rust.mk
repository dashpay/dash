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
$(package)_rust_std_targets += x86_64-unknown-freebsd
$(package)_rust_target_x86_64-unknown-freebsd:=x86_64-unknown-freebsd
$(package)_file_name_x86_64_freebsd:=rust-$($(package)_version)-x86_64-unknown-freebsd.tar.gz
$(package)_sha256_hash_x86_64_freebsd:=f7b51943dbed0af3387e3269c1767fee916fb22b8e7897b3594bf5e422403137
$(package)_rust_std_sha256_hash_x86_64-unknown-freebsd:=be1acaf3c2f15d42b05b1f032db5ac3b11a0ac5a91c0efef26f2d8135d68a829

# Linux (ARMv8)
$(package)_rust_std_targets += aarch64-unknown-linux-gnu
$(package)_rust_target_aarch64-unknown-linux-gnu:=aarch64-unknown-linux-gnu
$(package)_file_name_aarch64_linux:=rust-$($(package)_version)-aarch64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_aarch64_linux:=d7db04fce65b5f73282941f3f1df5893be9810af17eb7c65b2e614461fe31a48
$(package)_rust_std_sha256_hash_aarch64-unknown-linux-gnu:=82b2308ee531775bf4d1faa57bddfae85f363bec43ca36ba6db4ebad7c1450d4

# Linux (x86_64)
$(package)_rust_std_targets += x86_64-unknown-linux-gnu
$(package)_rust_target_x86_64-unknown-linux-gnu:=x86_64-unknown-linux-gnu
$(package)_file_name_x86_64_linux:=rust-$($(package)_version)-x86_64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_x86_64_linux:=0265c08ae997c4de965048a244605fb1f24a600bbe35047b811c638b8fcf676b
$(package)_rust_std_sha256_hash_x86_64-unknown-linux-gnu:=e7e808b8745298369fa3bbc3c0b7af9ca0fb995661bd684a7022d14bc9ae0057

# macOS (x86_64)
$(package)_rust_std_targets += x86_64-apple-darwin
$(package)_rust_target_x86_64-apple-darwin:=x86_64-apple-darwin
$(package)_file_name_x86_64_darwin:=rust-$($(package)_version)-x86_64-apple-darwin.tar.gz
$(package)_sha256_hash_x86_64_darwin:=b1a289cabc523f259f65116a41374ac159d72fbbf6c373bd5e545c8e835ceb6a
$(package)_rust_std_sha256_hash_x86_64-apple-darwin:=52084c8cdb34ca139a00f9f03f1a582d96b677e9f223a8d1aa31ae575a06cc16

# macOS (ARMv8)
$(package)_rust_std_targets += aarch64-apple-darwin
$(package)_rust_target_aarch64-apple-darwin:=aarch64-apple-darwin
$(package)_rust_target_arm64-apple-darwin:=aarch64-apple-darwin
$(package)_file_name_aarch64_darwin:=rust-$($(package)_version)-aarch64-apple-darwin.tar.gz
$(package)_sha256_hash_aarch64_darwin:=49b6d36b308addcfd21ae56c94957688338ba7b8985bff57fc626c8e1b32f62c
$(package)_rust_std_sha256_hash_aarch64-apple-darwin:=5ec28e75ed8715efaa2490d76ae026a34b13df6899d98b14d0a6995556f4e6b4

# Linux (ARMv7) - Cross only
$(package)_rust_std_targets += armv7-unknown-linux-gnueabihf
$(package)_rust_target_arm-unknown-linux-gnueabihf:=armv7-unknown-linux-gnueabihf
$(package)_rust_target_armv7-unknown-linux-gnueabihf:=armv7-unknown-linux-gnueabihf
$(package)_rust_std_sha256_hash_armv7-unknown-linux-gnueabihf:=5dd8b36467e03ba47bfa7ea5d7578c66bccb648dd2129d7cec6fb3ff00f81ca3

# Linux (PowerPC 64-bit) - Cross only
$(package)_rust_std_targets += powerpc64le-unknown-linux-gnu
$(package)_rust_target_powerpc64le-unknown-linux-gnu:=powerpc64le-unknown-linux-gnu
$(package)_rust_std_sha256_hash_powerpc64le-unknown-linux-gnu:=142c7c2896fa4596b5c4c35d9d5e4d80acd5a699e5fa0560d92a89eda035ece3

# Linux (RISCV64 GC) - Cross only
$(package)_rust_std_targets += riscv64gc-unknown-linux-gnu
$(package)_rust_target_riscv64-unknown-linux-gnu:=riscv64gc-unknown-linux-gnu
$(package)_rust_target_riscv64gc-unknown-linux-gnu:=riscv64gc-unknown-linux-gnu
$(package)_rust_std_sha256_hash_riscv64gc-unknown-linux-gnu:=7b35c8207c77e3fc2f7f7a26dea989cc2cdc13a955851ff74d4882f96f4e14dd

# Windows (x86_64) - Cross only
$(package)_rust_std_targets += x86_64-pc-windows-gnu
$(package)_rust_target_x86_64-w64-mingw32:=x86_64-pc-windows-gnu
$(package)_rust_target_x86_64-pc-windows-gnu:=x86_64-pc-windows-gnu
$(package)_rust_std_sha256_hash_x86_64-pc-windows-gnu:=32d42270b114c9341e5bc9b1d24f336024889ddd32a7d22e4700cc3c45fe9d3d

###
$(package)_file_name:=$($(package)_file_name_$(build_arch)_$(build_os))
$(package)_sha256_hash:=$($(package)_sha256_hash_$(build_arch)_$(build_os))
$(package)_rust_target:=$(or $($(package)_rust_target_$(canonical_host)),$($(package)_rust_target_$(subst -pc-,-unknown-,$(canonical_host))),$($(package)_rust_target_$(subst -unknown-,-pc-,$(canonical_host))))
###

define $(package)_set_vars
$(package)_stage_opts=--disable-ldconfig
$(package)_stage_build_opts=--without=rust-docs-json-preview,rust-docs
endef

ifneq ($(canonical_host),$(build))
$(package)_exact_file_name:=rust-std-$($(package)_version)-$($(package)_rust_target).tar.gz
$(package)_exact_sha256_hash:=$($(package)_rust_std_sha256_hash_$($(package)_rust_target))
$(package)_build_subdir:=buildos
$(package)_extra_sources:=$($(package)_exact_file_name)

define $(package)_fetch_cmds
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_exact_file_name),$($(package)_exact_file_name),$($(package)_exact_sha256_hash)) && \
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_file_name_$(build_arch)_$(build_os)),$($(package)_file_name_$(build_arch)_$(build_os)),$($(package)_sha256_hash_$(build_arch)_$(build_os)))
endef

define $(package)_extract_cmds
  mkdir -p $($(package)_extract_dir) && \
  echo "$($(package)_exact_sha256_hash)  $($(package)_source_dir)/$($(package)_exact_file_name)" > $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  echo "$($(package)_sha256_hash_$(build_arch)_$(build_os))  $($(package)_source_dir)/$($(package)_file_name_$(build_arch)_$(build_os))" >> $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  $(build_SHA256SUM) -c $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  mkdir $(canonical_host) && \
  $(build_TAR) -P --no-same-owner --strip-components=1 -xf $($(package)_source_dir)/$($(package)_exact_file_name) -C $(canonical_host) && \
  mkdir buildos && \
  $(build_TAR) -P --no-same-owner --strip-components=1 -xf $($(package)_source_dir)/$($(package)_file_name_$(build_arch)_$(build_os)) -C buildos
endef

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_dir)/$(host_prefix)/native/bin && \
  mkdir -p $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib && \
  cp cargo/bin/cargo $($(package)_staging_dir)/$(host_prefix)/native/bin/ && \
  cp rustc/bin/rustc $($(package)_staging_dir)/$(host_prefix)/native/bin/ && \
  cp rustc/bin/rustdoc $($(package)_staging_dir)/$(host_prefix)/native/bin/ && \
  cp -r rustc/lib/* $($(package)_staging_dir)/$(host_prefix)/native/lib/ && \
  cp -r rust-std-*/lib/rustlib/* $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib/ && \
  cp -r ../$(canonical_host)/rust-std-$($(package)_rust_target)/lib/rustlib/$($(package)_rust_target) $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib/ && \
  bash $(BASEDIR)/patches/native_rust/fix-elf-interpreter.sh \
    $($(package)_staging_dir)/$(host_prefix)/native/lib \
    $($(package)_staging_dir)/$(host_prefix)/native/bin/cargo \
    $($(package)_staging_dir)/$(host_prefix)/native/bin/rustc \
    $($(package)_staging_dir)/$(host_prefix)/native/bin/rustdoc
endef
else

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
endif
