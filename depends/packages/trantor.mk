package=trantor
$(package)_version=1.5.26
$(package)_download_path=https://github.com/an-tao/trantor/archive/refs/tags
$(package)_download_file=v$($(package)_version).tar.gz
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=e47092938aaf53d51c8bc72d8f54ebdcf537e6e4ac9c8276f3539413d6dfeddf
$(package)_build_subdir=build

define $(package)_set_vars
  $(package)_config_opts := -DCMAKE_BUILD_TYPE=None
  $(package)_config_opts += -DBUILD_C-ARES=OFF
  $(package)_config_opts += -DBUILD_SHARED_LIBS=OFF
  $(package)_config_opts += -DBUILD_TESTING=OFF
  $(package)_config_opts += -DTRANTOR_USE_TLS=none
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
  rm -rf lib/pkgconfig
endef
