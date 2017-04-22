Owncoin Core staging tree 0.12.1
===============================

`master:` [![Build Status](https://travis-ci.org/dmrtsvetkov/owncoin.svg?branch=master)](https://travis-ci.org/dmrtsvetkov/owncoin) `v0.12.0.x:` [![Build Status](https://travis-ci.org/dmrtsvetkov/owncoin.svg?branch=v0.12.0.x)](https://travis-ci.org/dmrtsvetkov/owncoin/branches) `v0.12.1.x:` [![Build Status](https://travis-ci.org/dmrtsvetkov/owncoin.svg?branch=v0.12.1.x)](https://travis-ci.org/dmrtsvetkov/owncoin/branches)
<<<<<<< HEAD
=======

https://www.owncoin.org
>>>>>>> refs/remotes/dmrtsvetkov/v0.12.0.x

https://www.owncoin.org


What is Owncoin?
----------------

Owncoin is an experimental new digital currency that enables anonymous, instant
payments to anyone, anywhere in the world. Owncoin uses peer-to-peer technology
to operate with no central authority: managing transactions and issuing money
are carried out collectively by the network. Owncoin Core is the name of the open
source software which enables the use of this currency.

For more information, as well as an immediately useable, binary version of
<<<<<<< HEAD
the Owncoin Core software, see https://www.owncoin.org/get-owncoin/.
=======
the Owncoin Core software, see https://www.owncoin.org/downloads.
>>>>>>> refs/remotes/dmrtsvetkov/v0.12.0.x


License
-------

Owncoin Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is meant to be stable. Development is normally done in separate branches.
[Tags](https://github.com/dmrtsvetkov/owncoin/tags) are created to indicate new official,
stable release versions of Owncoin Core.

<<<<<<< HEAD
The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md).
=======
The contribution workflow is described in [CONTRIBUTING.md](https://github.com/dmrtsvetkov/owncoin/blob/v0.12.1.x/CONTRIBUTING.md).

>>>>>>> refs/remotes/dmrtsvetkov/v0.12.0.x

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

Translations
------------

Changes to translations as well as new translations can be submitted to
[Owncoin Core's Transifex page](https://www.transifex.com/projects/p/owncoin/).

Translations are periodically pulled from Transifex and merged into the git repository. See the
[translation process](doc/translation_process.md) for details on how this works.

**Important**: We do not accept translation changes as GitHub pull requests because the next
pull from Transifex would automatically overwrite them again.

Translators should also follow the [forum](https://www.owncoin.org/forum/topic/owncoin-worldwide-collaboration.88/).
