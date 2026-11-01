# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

package=native_protobuf
$(package)_version=32.0
$(package)_download_path=https://github.com/protocolbuffers/protobuf/releases/download/v$($(package)_version)

# Linux (ARMv8)
$(package)_file_name_aarch64_linux=protoc-$($(package)_version)-linux-aarch_64.zip
$(package)_sha256_hash_aarch64_linux=56af3fc2e43a0230802e6fadb621d890ba506c5c17a1ae1070f685fe79ba12d0

# Linux (x86_64)
$(package)_file_name_x86_64_linux=protoc-$($(package)_version)-linux-x86_64.zip
$(package)_sha256_hash_x86_64_linux=7ca037bfe5e5cabd4255ccd21dd265f79eb82d3c010117994f5dc81d2140ee88

# macOS (ARMv8)
$(package)_file_name_aarch64_darwin=protoc-$($(package)_version)-osx-aarch_64.zip
$(package)_sha256_hash_aarch64_darwin=09a2c729cc821215cc0d4c564b761760961fe338c52f24b302fd7e18e7b675d1

# macOS (x86_64)
$(package)_file_name_x86_64_darwin=protoc-$($(package)_version)-osx-x86_64.zip
$(package)_sha256_hash_x86_64_darwin=63eeba15ddc12ab11b0a8bce81fb2d46cc69022c3e6ad21fecde90d52139bff6

$(package)_file_name=$($(package)_file_name_$(build_arch)_$(build_os))
$(package)_sha256_hash=$($(package)_sha256_hash_$(build_arch)_$(build_os))

ifeq ($($(package)_file_name),)
$(error native_protobuf has no prebuilt protoc $($(package)_version) for $(build_arch)-$(build_os))
endif

define $(package)_extract_cmds
  echo "$($(package)_sha256_hash)  $($(package)_source)" > .$($(package)_file_name).hash && \
  $(build_SHA256SUM) -c .$($(package)_file_name).hash && \
  python3 -m zipfile -e $($(package)_source) .
endef

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_prefix_dir)/bin $($(package)_staging_prefix_dir)/include && \
  cp bin/protoc $($(package)_staging_prefix_dir)/bin/ && \
  chmod 0755 $($(package)_staging_prefix_dir)/bin/protoc && \
  cp -R include/google $($(package)_staging_prefix_dir)/include/
endef
