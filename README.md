# LotteryCoin

## Developers

Extra info in: `~/doc/build_unix.md`, but it's pretty hard to follow.

### Installation

Install dependencies:

    // Source link https://github.com/bitcoin/bitcoin/issues/7970
    $ sudo add-apt-repository ppa:bitcoin/bitcoin
    $ sudo apt-get update
    $ sudo apt-get install libdb4.8-dev libdb4.8++-dev

    // git clone this repo
    // cd into the lotterycoin directory root
    $ ./autogen.sh
    $ ./configure
    $ make

The last command takes a freaking age (5 to 10 minutes) and produces a lot of `CXX libbitcoin ...` and warning messages, but if it's still running then let it do its thing.

    $ sudo make install

Now set up the the terminal daemon:

    $ sudo apt-get install libqt5gui5 libqt5core5a libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler

    // Source link https://www.dash.org/forum/threads/install-dashcoind-to-ubuntu-14-04.4827/
    // cd into lotterycoin/src directory
    $ ./dashd --daemon

    // Now you should have the terminal version running
    // Check if it's running on port 9998 or 9999
    $ lsof wni
    // You can also hit localhost:9998 in the browser
    // This message means it's working `JSONRPC server handles only POST requests`
    // With the daemon running you can use RPC commands
    $ ./dash-cli help
    $ ./dash-cli getwalletinfo
    // Use Ctrl+C to kill the daemon, you can't run it at the same time as the wallet
    // Alternative way to shut off the daemon
    $ lsof wni
    // Find the port number for `dashd`
    $ kill <port number>

Ok now to switch to the GUI wallet:

    // cd into lotterycoin/src/qt
    $ ./dash-qt

The wallet should start up on your screen.

### Developing

UI files are in `src/qt/forms`

## License

Copyright LotteryCoin Developers 2018
