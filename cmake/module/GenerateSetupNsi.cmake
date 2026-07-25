# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(generate_setup_nsi)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  set(abs_top_builddir ${PROJECT_BINARY_DIR})
  set(PACKAGE_URL ${PROJECT_HOMEPAGE_URL})
  # These have to match the executables' OUTPUT_NAMEs, since the deploy target
  # stages the files under those names. Keep them in sync with configure.ac.
  # PACKAGE_TARNAME comes from the top-level CMakeLists.txt, which is also where
  # add_windows_deploy_target() reads it from.
  set(BITCOIN_GUI_NAME "dash-qt")
  set(BITCOIN_DAEMON_NAME "dashd")
  set(BITCOIN_CLI_NAME "dash-cli")
  set(BITCOIN_TX_NAME "dash-tx")
  set(BITCOIN_WALLET_TOOL_NAME "dash-wallet")
  set(BITCOIN_TEST_NAME "test_dash")
  set(EXEEXT ${CMAKE_EXECUTABLE_SUFFIX})
  set(nsi_file ${PROJECT_BINARY_DIR}/${PACKAGE_TARNAME}-win64-setup.nsi)
  configure_file(${PROJECT_SOURCE_DIR}/share/setup.nsi.in ${nsi_file} @ONLY)
  # share/setup.nsi.in carries no OutFile directive; the Autotools recipe pipes
  # one in ahead of the script when invoking makensis. Do the same here.
  file(READ ${nsi_file} nsi_content)
  file(WRITE ${nsi_file}
    "OutFile \"${PROJECT_BINARY_DIR}/${PACKAGE_TARNAME}-${PACKAGE_VERSION}-win64-setup${EXEEXT}\"\n${nsi_content}"
  )
endfunction()
