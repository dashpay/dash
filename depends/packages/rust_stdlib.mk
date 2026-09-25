# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Precompiled Rust standard libraries for the host and, when it differs, the
# build machine, installed next to the native_rust compiler. The version
# follows native_rust.mk; update both with
# ./contrib/devtools/update-rust-hashes.py

package:=rust_stdlib
$(package)_version:=$(native_rust_version)
$(package)_download_path:=$(native_rust_download_path)

# Every host in contrib/guix/guix-build's default HOSTS has an entry. Linux
# hosts use the glibc (-unknown-linux-gnu) standard library, the one Rust
# supports for linking into a glibc program. Its libc imports are unversioned
# in the archive and bind at link time to the glibc the program is linked
# against; every symbol it requires unconditionally exists in glibc 2.31 on
# all five Linux architectures (the rest are weak and looked up at run time).

# Linux (x86_64)
$(package)_target_x86_64_linux:=x86_64-unknown-linux-gnu
$(package)_sha256_hash_x86_64_linux:=fa3ff450172a16c026944030230c5069947af93c728d9179971d44e5e0cfb561

# Linux (ARMv8)
$(package)_target_aarch64_linux:=aarch64-unknown-linux-gnu
$(package)_sha256_hash_aarch64_linux:=9bf796a6ec5b004813ebd0b650775a7c6a4f3aae97ad362ae294798dca4f3b23

# Linux (RISC-V 64)
$(package)_target_riscv64_linux:=riscv64gc-unknown-linux-gnu
$(package)_sha256_hash_riscv64_linux:=bcbd81e49d72e6907c92ce7d1ba5b25b02bbd85f94cb21567c931aad1d3e3ab7

# Linux (ARMv7, hard float)
$(package)_target_arm_linux:=armv7-unknown-linux-gnueabihf
$(package)_sha256_hash_arm_linux:=0685de03c96ae65212b519d98116ad581c7ef46ca24acad1364895b7907d71be

# Linux (POWER, big endian)
$(package)_target_powerpc64_linux:=powerpc64-unknown-linux-gnu
$(package)_sha256_hash_powerpc64_linux:=f23c066e7bba9d2216e41b6add0cb429c7cb7f90b449918c5370fbcb88b0dd88

# Windows (x86_64)
$(package)_target_x86_64_mingw32:=x86_64-pc-windows-gnu
$(package)_sha256_hash_x86_64_mingw32:=5bb599a541fcb9c0edc00e512570f60d2262623f1e2a19a44cce3a7a97208788

# macOS (x86_64)
$(package)_target_x86_64_darwin:=x86_64-apple-darwin
$(package)_sha256_hash_x86_64_darwin:=48b9835058a8baa4ff5e4387f3772c572a017a2b13393c46e151615c54e322e9

# macOS (ARMv8)
$(package)_target_aarch64_darwin:=aarch64-apple-darwin
$(package)_sha256_hash_aarch64_darwin:=2de831ef563ce772519a4d18ba2659f0f7428d497ef66dad24b5a35e8f8cd177

$(package)_target:=$($(package)_target_$(host_arch)_$(host_os))
$(package)_sha256_hash:=$($(package)_sha256_hash_$(host_arch)_$(host_os))

ifeq ($($(package)_target),)
$(error rust_stdlib has no Rust standard library for $(host))
endif

$(package)_file_name:=rust-std-$($(package)_version)-$($(package)_target).tar.xz

# Build scripts and proc macros need the build machine's standard library;
# when it is not the host's, it comes as a second archive.
ifneq ($(build_arch)_$(build_os),$(host_arch)_$(host_os))
$(package)_build_target:=$($(package)_target_$(build_arch)_$(build_os))
$(package)_build_file_name:=rust-std-$($(package)_version)-$($(package)_build_target).tar.xz
$(package)_build_sha256_hash:=$($(package)_sha256_hash_$(build_arch)_$(build_os))
$(package)_extra_sources:=$($(package)_build_file_name)
$(package)_fetched_id:=$($(package)_build_sha256_hash)

define $(package)_fetch_cmds
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_file_name),$($(package)_file_name),$($(package)_sha256_hash)) && \
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_build_file_name),$($(package)_build_file_name),$($(package)_build_sha256_hash))
endef

define $(package)_extract_cmds
  echo "$($(package)_sha256_hash)  $($(package)_source)" > .$($(package)_file_name).hash && \
  echo "$($(package)_build_sha256_hash)  $($(package)_source_dir)/$($(package)_build_file_name)" >> .$($(package)_file_name).hash && \
  $(build_SHA256SUM) -c .$($(package)_file_name).hash && \
  $(build_TAR) --no-same-owner --strip-components=1 -xf $($(package)_source) && \
  $(build_TAR) --no-same-owner --strip-components=1 -xf $($(package)_source_dir)/$($(package)_build_file_name)
endef
endif

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_dir)$(build_prefix)/lib/rustlib && \
  $(foreach target,$($(package)_target) $($(package)_build_target),cp -R rust-std-$(target)/lib/rustlib/$(target) $($(package)_staging_dir)$(build_prefix)/lib/rustlib/ && ) true
endef
