Waggox Core staging tree 1.0.0.0
===============================

`master:` [![Build Status](https://travis-ci.org/dashpay/dash.svg?branch=master)](https://travis-ci.org/dashpay/dash) `develop:` [![Build Status](https://travis-ci.org/dashpay/dash.svg?branch=develop)](https://travis-ci.org/dashpay/dash/branches)

https://website.waggok.tech


What is Waggox?
----------------
Waggox is a fork of dash an experimental cryptocurrency with the aim of promoting good and secure transactions. It is a coin good for buying and selling of different services as well as goods.


SPECIFICATIONS
--------------
* Algorithm: x11/MN

* Block Reward: 8.11 WAGGOX

* RPCPORT: 9920

* P2P PORT: 9910

* Max. Supply: 8million Waggox

* Masternode Collateral: 5000 Waggox


License
-------

Waggox is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is meant to be stable. Development is normally done in separate branches.
[Tags](https://github.com/waggoxcoin/waggox/tags) are created to indicate new official,
stable release versions of waggox core.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md).

Testing
-------

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test on short notice. Please be patient and help out by testing
other people's pull requests, and remember this is a security-critical project where any mistake might cost people
lots of money.

### Automated Testing

Developers are strongly encouraged to write [unit tests](src/test/README.md) for new code, and to
submit new unit tests for old code. Unit tests can be compiled and run
(assuming they weren't disabled in configure) with: `make check`. Further details on running
and extending unit tests can be found in [/src/test/README.md](/src/test/README.md).

There are also [regression and integration tests](/qa) of the RPC interface, written
in Python, that are run automatically on the build server.
These tests can be run (if the [test dependencies](/qa) are installed) with: `qa/pull-tester/rpc-tests.py`

The Travis CI system makes sure that every pull request is built for Windows, Linux, and OS X, and that unit/sanity tests are run automatically.

### Manual Quality Assurance (QA) Testing

Changes should be tested by somebody other than the developer who wrote the
code. This is especially important for large or high-risk changes. It is useful
to add a test plan to the pull request description if testing the changes is
not straightforward.

Translations
------------

Changes to translations as well as new translations can be submitted to
[Waggox Core's Transifex page](https://www.transifex.com/projects/p/waggox/).

Translations are periodically pulled from Transifex and merged into the git repository. See the
[translation process](doc/translation_process.md) for details on how this works.

**Important**: We do not accept translation changes as GitHub pull requests because the next
pull from Transifex would automatically overwrite them again.

