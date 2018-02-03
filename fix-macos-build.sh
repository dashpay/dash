#!/bin/bash

install_name_tool -change "@rpath/libc++.1.dylib" "/usr/lib/libc++.1.dylib" Vbd-Qt.app/Contents/MacOS/Dash-Qt
