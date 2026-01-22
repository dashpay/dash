# Copyright (c) 2022-2025 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# To update the package, change the version below and then run the script
# ./contrib/devtools/update-native-cxxbridge.py

package:=native_cxxbridge
$(package)_version:=1.0.192
$(package)_download_path:=https://github.com/dtolnay/cxx/archive/refs/tags
$(package)_file_name:=native_cxxbridge-$($(package)_version).tar.gz
$(package)_download_file:=$($(package)_version).tar.gz
$(package)_sha256_hash:=d242dbfb6deb362c5a95fcb69aeb5f25228de75af3cced94c9f6c8e11019d30e
$(package)_build_subdir:=gen/cmd
$(package)_dependencies:=native_rust
$(package)_patches:=Cargo.lock cargo-config.toml
$(package)_vendored_file_name:=native_cxxbridge-$($(package)_version)-vendored.tar.gz
$(package)_cargo_manifest:=gen/cmd/Cargo.toml

define $(package)_preprocess_cmds
  cp $($(package)_patch_dir)/Cargo.lock .
endef

define $(package)_build_cmds
  $($(package)_cargo) build --locked --release --package=cxxbridge-cmd --bin=cxxbridge
endef

define $(package)_stage_cmds
  $($(package)_cargo) install --locked --path=. --bin=cxxbridge --root=$($(package)_staging_prefix_dir) && \
  mkdir -p $($(package)_staging_prefix_dir)/lib && \
  bash $(BASEDIR)/patches/native_rust/fix-elf-interpreter.sh \
    $($(package)_staging_prefix_dir)/lib \
    $($(package)_staging_prefix_dir)/bin/cxxbridge
endef
