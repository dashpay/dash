package=jsoncpp
$(package)_version=1.9.6
$(package)_download_path=https://github.com/open-source-parsers/jsoncpp/archive/refs/tags
$(package)_download_file=$($(package)_version).tar.gz
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=f93b6dd7ce796b13d02c108bc9f79812245a82e577581c4c9aabe57075c90ea2
$(package)_build_subdir=build

define $(package)_set_vars
  $(package)_config_opts := -DCMAKE_BUILD_TYPE=None
  $(package)_config_opts += -DBUILD_SHARED_LIBS=OFF
  $(package)_config_opts += -DBUILD_STATIC_LIBS=ON
  $(package)_config_opts += -DJSONCPP_WITH_CMAKE_PACKAGE=ON
  $(package)_config_opts += -DJSONCPP_WITH_POST_BUILD_UNITTEST=OFF
  $(package)_config_opts += -DJSONCPP_WITH_TESTS=OFF
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
