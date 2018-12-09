Cadex Core staging tree 1.0.0
===============================

`master:` [![Build Status](https://travis-ci.org/dashpay/dash.svg?branch=master)](https://travis-ci.org/dashpay/dash) `develop:` [![Build Status](https://travis-ci.org/dashpay/dash.svg?branch=develop)](https://travis-ci.org/dashpay/dash/branches)

http://cadex.xyz


What is Cadex?
----------------

Cadex is a totl of dash fork an experimental digital currency that enables anonymous, instant
payments to anyone, anywhere in the world. Cadex uses peer-to-peer technology
to operate with no central authority: managing transactions and issuing money
are carried out collectively by the network. Dash Core is the name of the open
source software which enables the use of this currency.

SPECIFICATIONS
--------------

MASTERNODE CONFIGURATIONS
-------------------------
1) Using the control wallet, enter the debug console (Tools > Debug console) and type the following command:
```
masternode genkey (This will be the masternode’s privkey. We’ll use this later…)
```
2) Using the control wallet still, enter the following command:
```
getaccountaddress chooseAnyNameForYourMasternode
```
3) Still in the control wallet, send 100,000 CADEX to the address you generated in step 2 (Be 100% sure that you entered the address correctly. You can verify this when you paste the address into the “Pay To:” field, the label will autopopulate with the name you chose”, also make sure this is exactly 100,000 CADEX; No less, no more.)
4) Still in the control wallet, enter the command into the console (This gets the proof of transaction of sending 10,000):
```
masternode outputs
```
5) Still on the main computer, go into the cadex data directory, by default in Windows it’ll be
```
%Appdata%/CADEXCOIN
```
or Linux
```
cd ~/.CADEXCOIN
```
Find masternode.conf and add the following line to it:
```
# Format: alias IP:port masternodeprivkey collateral_output_txid collateral_output_index
```
or follow here
```
<Name of Masternode(Use the name you entered earlier for simplicity)> <Unique IP address>:51472 <The result of Step 1> <Result of Step 4> <The number after the long line in Step 4>
```
B. VPS Remote wallet install
----------------------------



License
-------

Dash Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is meant to be stable. Development is normally done in separate branches.
[Tags](https://github.com/cadexproject/cadex/tags) are created to indicate new official,
stable release versions of Dash Core.

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
[Dash Core's Transifex page](https://www.transifex.com/projects/p/cadex/).

Translations are periodically pulled from Transifex and merged into the git repository. See the
[translation process](doc/translation_process.md) for details on how this works.

**Important**: We do not accept translation changes as GitHub pull requests because the next
pull from Transifex would automatically overwrite them again.

