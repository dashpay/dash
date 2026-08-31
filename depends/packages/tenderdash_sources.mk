# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Source-only package: the tenderdash source archive that tenderdash-proto's
# build.rs (rs-tenderdash-abci, a dependency of the Platform CXX package)
# compiles its protobuf definitions from.
# Online builds download this zip themselves at cargo build time; depends
# builds must not touch the network, so the sha256-pinned archive is staged
# verbatim into the prefix and platform_cxx copies it into the Cargo target
# directory, where build.rs treats it as a pre-populated download
# cache (cache file name: tenderdash-$(TENDERDASH_COMMITISH).zip).
#
# The version must match TENDERDASH_COMMITISH default of the pinned
# tenderdash-proto crate (rs-tenderdash-abci proto/build.rs).

package=tenderdash_sources
$(package)_version=1.5.1
$(package)_download_path=https://github.com/dashpay/tenderdash/archive
$(package)_download_file=v$($(package)_version).zip
$(package)_file_name=tenderdash-v$($(package)_version).zip
$(package)_sha256_hash=7a8844899a4635a6c2f55057e0c0f7cec357907d0cfeb2900e035760cf187f9a

# Keep the archive as-is: the consumer (tenderdash-proto build.rs) unzips it
# from its own cache directory, so extraction here would only be discarded.
define $(package)_extract_cmds
  mkdir -p $($(package)_extract_dir) && \
  echo "$($(package)_sha256_hash)  $($(package)_source)" > $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  $(build_SHA256SUM) -c $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  cp $($(package)_source) $($(package)_file_name)
endef

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_dir)/$(host_prefix)/tenderdash-sources && \
  cp $($(package)_file_name) $($(package)_staging_dir)/$(host_prefix)/tenderdash-sources/
endef
