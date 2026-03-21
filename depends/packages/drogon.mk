package=drogon
$(package)_version=1.9.12
$(package)_download_path=https://github.com/drogonframework/drogon/archive/refs/tags
$(package)_download_file=v$($(package)_version).tar.gz
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=becc3c4f3b90f069f814baef164a7e3a2b31476dc6fe249b02ff07a13d032f48
$(package)_dependencies=jsoncpp trantor zlib
$(package)_build_subdir=build

define $(package)_set_vars
  $(package)_config_opts := -DCMAKE_BUILD_TYPE=None
  $(package)_config_opts += -DBUILD_BROTLI=OFF
  $(package)_config_opts += -DBUILD_CTL=OFF
  $(package)_config_opts += -DBUILD_EXAMPLES=OFF
  $(package)_config_opts += -DBUILD_ORM=OFF
  $(package)_config_opts += -DBUILD_REDIS=OFF
  $(package)_config_opts += -DBUILD_SHARED_LIBS=OFF
  $(package)_config_opts += -DBUILD_TESTING=OFF
  $(package)_config_opts += -DBUILD_YAML_CONFIG=OFF
  $(package)_config_opts += -DUSE_SUBMODULE=OFF
  $(package)_cxxflags += -fdebug-prefix-map=$($(package)_extract_dir)=/usr -fmacro-prefix-map=$($(package)_extract_dir)=/usr
endef

define $(package)_config_cmds
  $($(package)_cmake) -S .. -B .
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_postprocess_cmds
  rm -rf lib/cmake && \
  rm -rf lib/pkgconfig
endef
