# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# The rustc and cargo components of the prebuilt Rust toolchain. rust_stdlib
# installs the standard libraries next to them. To update the Rust compiler,
# change the version below and then run the script
# ./contrib/devtools/update-rust-hashes.py

package:=native_rust
$(package)_version:=1.98.1
$(package)_download_path:=https://static.rust-lang.org/dist

# Linux (ARMv8)
$(package)_build_target_aarch64_linux:=aarch64-unknown-linux-gnu
$(package)_rustc_sha256_hash_aarch64_linux:=89fb83041993b48816514815606f53a5264729b8b449671a6b291e6f0ae74f40
$(package)_cargo_sha256_hash_aarch64_linux:=c09425a7f300af148c0bdfded0e7d5f1fe7c075b2b9674e8935afa769297b2f6

# Linux (x86_64)
$(package)_build_target_x86_64_linux:=x86_64-unknown-linux-gnu
$(package)_rustc_sha256_hash_x86_64_linux:=e974f036b28565f37c0f3bd92ddefa809bee16c04f9dcf07b9ed96e05aaaf7c4
$(package)_cargo_sha256_hash_x86_64_linux:=ea1de9f9e23107d97ee2b41a72c552f34064a593da503789218387aee59f3ba4

# macOS (ARMv8)
$(package)_build_target_aarch64_darwin:=aarch64-apple-darwin
$(package)_rustc_sha256_hash_aarch64_darwin:=738e3f60114b20550fcf4f8a57c9cea0544239f214709f45594ff3fde327f577
$(package)_cargo_sha256_hash_aarch64_darwin:=de51d4fade4f31ad8ec405261cc96bedc000d905f98980027610d6e288dbe21b

# macOS (x86_64)
$(package)_build_target_x86_64_darwin:=x86_64-apple-darwin
$(package)_rustc_sha256_hash_x86_64_darwin:=641c301bd19c3cead114cbb61919c5ed2475ce53e2d011fe0c990e16c5a1b04b
$(package)_cargo_sha256_hash_x86_64_darwin:=74b1282253c31fb89926d51d57e47a06c6cd26c93657de72edf622a4ab66396f

# The Rust target triple of the build machine.
$(package)_build_target:=$($(package)_build_target_$(build_arch)_$(build_os))

ifeq ($($(package)_build_target),)
$(error native_rust has no prebuilt Rust $($(package)_version) for $(build_arch)-$(build_os))
endif

$(package)_file_name:=rustc-$($(package)_version)-$($(package)_build_target).tar.xz
$(package)_sha256_hash:=$($(package)_rustc_sha256_hash_$(build_arch)_$(build_os))
$(package)_cargo_file_name:=cargo-$($(package)_version)-$($(package)_build_target).tar.xz
$(package)_cargo_sha256_hash:=$($(package)_cargo_sha256_hash_$(build_arch)_$(build_os))
$(package)_fetched_id:=$($(package)_cargo_sha256_hash)
$(package)_extra_sources:=$($(package)_cargo_file_name)

# The prebuilt toolchain cannot run inside a Guix environment as is.
ifneq ($(GUIX_ENVIRONMENT),)
$(error native_rust: PLATFORM_GUI is not supported in Guix builds yet)
endif

define $(package)_fetch_cmds
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_file_name),$($(package)_file_name),$($(package)_sha256_hash)) && \
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_cargo_file_name),$($(package)_cargo_file_name),$($(package)_cargo_sha256_hash))
endef

define $(package)_extract_cmds
  echo "$($(package)_sha256_hash)  $($(package)_source)" > .$($(package)_file_name).hash && \
  echo "$($(package)_cargo_sha256_hash)  $($(package)_source_dir)/$($(package)_cargo_file_name)" >> .$($(package)_file_name).hash && \
  $(build_SHA256SUM) -c .$($(package)_file_name).hash && \
  $(build_TAR) --no-same-owner --strip-components=1 -xf $($(package)_source) && \
  $(build_TAR) --no-same-owner --strip-components=1 -xf $($(package)_source_dir)/$($(package)_cargo_file_name)
endef

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_prefix_dir)/bin $($(package)_staging_prefix_dir)/lib && \
  cp cargo/bin/cargo rustc/bin/rustc $($(package)_staging_prefix_dir)/bin/ && \
  cp -R rustc/lib/. $($(package)_staging_prefix_dir)/lib/
endef
