package=mbedtls
$(package)_version=3.6.3.1
$(package)_download_path=https://github.com/Mbed-TLS/mbedtls/releases/download/v$($(package)_version)/
$(package)_file_name=$(package)-$($(package)_version).tar.bz2
$(package)_sha256_hash=243ed496d5f88a5b3791021be2800aac821b9a4cc16e7134aa413c58b4c20e0c

define $(package)_set_vars
$(package)_config_opts := -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF
$(package)_config_opts += -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON
$(package)_config_opts += -DMBEDTLS_FATAL_WARNINGS=OFF -DGEN_FILES=OFF
endef

define $(package)_config_cmds
  $($(package)_cmake) -S . -B .
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_postprocess_cmds
  rm -rf lib/cmake
endef
