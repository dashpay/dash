$PAC Core staging tree 0.12.3.0
===============================

<!-- `master:` [![Build Status](https://travis-ci.org/paccoinpay/paccoin.svg?branch=master)](https://travis-ci.org/paccoinpay/paccoin) `develop:` [![Build Status](https://travis-ci.org/paccoinpay/paccoin.svg?branch=develop)](https://travis-ci.org/paccoinpay/paccoin/branches) -->

https://www.paccoin.net


What is $PAC?
----------------

$PAC is an experimental new digital currency that enables anonymous, instant
payments to anyone, anywhere in the world. $PAC uses peer-to-peer technology
to operate with no central authority: managing transactions and issuing money
are carried out collectively by the network. $PAC is the name of the open
source software which enables the use of this currency.

For more information, as well as an immediately useable, binary version of
the $PAC software, see https://wallets.paccoin.net/.


License
-------

$PAC is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is meant to be stable. Development is normally done in separate branches.
[Tags](https://github.com/paccoinpay/paccoin/tags) are created to indicate new official,
stable release versions of $PAC.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md).

Testing
-------

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test on short notice. Please be patient and help out by testing
other people's pull requests, and remember this is a security-critical project where any mistake might cost people
lots of money.

### Automated Testing

Developers are strongly encouraged to write [unit tests](/doc/unit-tests.md) for new code, and to
submit new unit tests for old code. Unit tests can be compiled and run
(assuming they weren't disabled in configure) with: `make check`

There are also [regression and integration tests](/qa) of the RPC interface, written
in Python, that are run automatically on the build server.
These tests can be run (if the [test dependencies](/qa) are installed) with: `qa/pull-tester/rpc-tests.py`

The Travis CI system makes sure that every pull request is built for Windows
and Linux, OS X, and that unit and sanity tests are automatically run.

### Manual Quality Assurance (QA) Testing

Changes should be tested by somebody other than the developer who wrote the
code. This is especially important for large or high-risk changes. It is useful
to add a test plan to the pull request description if testing the changes is
not straightforward.

<!-- Translations
------------

Testing
-------
Changes to translations as well as new translations can be submitted to
[Paccoin Core's Transifex page](https://www.transifex.com/projects/p/paccoin/).

Translations are periodically pulled from Transifex and merged into the git repository. See the
[translation process](doc/translation_process.md) for details on how this works.

**Important**: We do not accept translation changes as GitHub pull requests because the next
pull from Transifex would automatically overwrite them again.

Translators should also follow the [forum](https://www.paccoin.org/forum/topic/paccoin-worldwide-collaboration.88/).
-->

Setting up a masternode on a VPS (only for Cold wallet setup)
-------
Go to your VPS and execute this:
1) `wget https://raw.githubusercontent.com/PACCommunity/PAC/master/pacmn.sh`
2) `chmod +x pacmn.sh`
3) `./pacmn.sh`

Follow the instructions and:
Enter your external IP
Enter your masternode genkey
The script will setup the environment and run your masternode 

Updating a masternode on a VPS to the latest verion (only for Cold wallet setup)
-------
Go to your VPS and execute the following commands on the command line:
1) `wget https://raw.githubusercontent.com/PACCommunity/PAC/master/pac-update.sh`
2) `chmod +x pac-update.sh`
3) `./pac-update.sh`

Or run this single line to execute the previous commands in one go:

`wget https://raw.githubusercontent.com/PACCommunity/PAC/master/pac-update.sh && chmod +x pac-update.sh && ./pac-update.sh`


Follow the instructions:
1)Do you want to autobackup wallet.dat and continue with the process? [y/n]: Type yes to backup and continue or no to stop process 
