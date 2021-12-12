package=cmake
$(package)_version=3.22.1
$(package)_download_path=https://cmake.org/files/v3.22/
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=0e998229549d7b3f368703d20e248e7ee1f853910d42704aa87918c213ea82c0

define $(package)_config_cmds
  export CC="" && \
  export CXX="" && \
  ./bootstrap --prefix=$(host_prefix)
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef
