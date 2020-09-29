Gitian building
================

*Setup instructions for a Gitian build of Dash Core using a VM or physical system.*

Gitian is the deterministic build process that is used to build the Dash
Core executables. It provides a way to be reasonably sure that the
executables are really built from the git source. It also makes sure that
the same, tested dependencies are used and statically built into the executable.

Multiple developers build the source code by following a specific descriptor
("recipe"), cryptographically sign the result, and upload the resulting signature.
These results are compared and only if they match, the build is accepted and provided
for download.

More independent Gitian builders are needed, which is why this guide exists.
It is preferred you follow these steps yourself instead of using someone else's
VM image to avoid 'contaminating' the build.


Preparing the Gitian builder host
---------------------------------

The first step is to prepare the host environment that will be used to perform the Gitian builds.
This guide explains how to set up the environment, and how to start the builds.

Gitian builds are known to be working on recent versions of Debian and Ubuntu.
If your machine is already running one of those operating systems, you can perform Gitian builds on the actual hardware.
Alternatively, you can install one of the supported operating systems in a virtual machine.

Any kind of virtualization can be used, for example:
- [VirtualBox](https://www.virtualbox.org/) (covered by this guide)
- [KVM](http://www.linux-kvm.org/page/Main_Page)
- [LXC](https://linuxcontainers.org/)

Please refer to the following documents to set up the operating systems and Gitian.

|                                   | Debian                                                                             
|-----------------------------------|------------------------------------------------------------------------------------
| Setup virtual machine (optional)  | [Create Debian VirtualBox](./gitian-building/gitian-building-create-vm-debian.md)
| Setup Gitian                      | [Setup Gitian on Debian](./gitian-building/gitian-building-setup-gitian-debian.md)

Note that a version of `lxc-execute` higher or equal to 2.1.1 is required.
You can check the version with `lxc-execute --version`.
On Debian you might have to compile a suitable version of lxc or you can use Ubuntu 18.04 or higher instead of Debian as the host.

Non-Debian / Ubuntu, Manual and Offline Building
------------------------------------------------
The instructions below use the automated script [gitian-build.py](https://github.com/bitcoin/bitcoin/blob/master/contrib/gitian-build.py) which only works in Debian/Ubuntu. For manual steps and instructions for fully offline signing, see [this guide](./gitian-building/gitian-building-manual.md).

MacOS code signing
------------------
In order to sign builds for macOS, you need to download the free SDK and extract a file. The steps are described [here](./gitian-building/gitian-building-mac-os-sdk.md). Alternatively, you can skip the macOS build by adding `--os=lw` below.

Initial Gitian Setup
--------------------
The `gitian-build.py` script will checkout different release tags, so it's best to copy it:

```bash
cp dash/contrib/gitian-build.py .
```

You only need to do this once:

```
./gitian-build.py --setup
```

In order to sign gitian builds on your host machine, which has your PGP key, fork the (gitian.sigs repository)[https://github.com/dashpay/gitian.sigs] and clone it on your host machine:

```
export NAME=satoshi
git clone git@github.com:$NAME/gitian.sigs.git
git remote add $NAME git@github.com:$NAME/gitian.sigs.git
```

Where `satoshi` is your GitHub name.

Build binaries
-----------------------------
Windows and macOS have code signed binaries, but those won't be available until a few developers have gitian signed the non-codesigned binaries.

To build the most recent tag:
```
 export NAME=satoshi
 export VERSION=0.16.0.0
 ./gitian-build.py --detach-sign --no-commit -b $NAME $VERSION
```

Where `0.16.0.0` is the most recent tag / version you want to build (without `v`).

To speed up the build, use `-j 5 -m 5000` as the first arguments, where `5` is the number of CPU cores you allocated to the VM plus one, and `5000` is a little bit less than the MBs of RAM you allocated.

If all went well, this produces a number of (uncommited) `.assert` files in the gitian.sigs repository.

You need to copy these uncommited changes to your host machine, where you can sign them:

```
gpg --output ${VERSION}-linux/${NAME}/dashcore-linux-${VERSION%\.*}-build.assert.sig --detach-sign ${VERSION}-linux/$NAME/dashcore-linux-${VERSION%\.*}-build.assert
gpg --output ${VERSION}-osx-unsigned/$NAME/dashcore-osx-${VERSION%\.*}-build.assert.sig --detach-sign ${VERSION}-osx-unsigned/$NAME/dashcore-osx-${VERSION%\.*}-build.assert
gpg --output ${VERSION}-win-unsigned/$NAME/dashcore-win-${VERSION%\.*}-build.assert.sig --detach-sign ${VERSION}-win-unsigned/$NAME/dashcore-win-${VERSION%\.*}-build.assert
```

Make a PR (both the `.assert` and `.assert.sig` files) to the
[dashpay/gitian.sigs](https://github.com/dashpay/gitian.sigs/) repository:

```
git checkout -b ${VERSION}-not-codesigned
git commit -S -a -m "Add $NAME $VERSION non-code signed signatures"
git push --set-upstream $NAME $VERSION-not-codesigned
```

```bash
    gpg --detach-sign ${VERSION}-linux/${NAME}/dashcore-linux-*-build.assert
    gpg --detach-sign ${VERSION}-win-unsigned/${NAME}/dashcore-win-*-build.assert
    gpg --detach-sign ${VERSION}-osx-unsigned/${NAME}/dashcore-osx-*-build.assert
```

You may have other .assert files as well (e.g. `signed` ones), in which case you should sign them too. You can see all of them by doing `ls ${VERSION}-*/${NAME}`.

This will create the `.sig` files that can be committed together with the `.assert` files to assert your
Gitian build.


 `./gitian-build.py --detach-sign -s $NAME $VERSION --nocommit`

Make another pull request for these.

Simplified Guide
-----------------------------

Prerequesites: 
 1) have a Github Account
 2) have a known PGP key, like on keybase
 3) be running ubuntu or similar
 4) have some command line experience

Step 1: 
Go to https://github.com/dashpay/gitian.sigs and hit "Fork" in the upper left, and wait for github to finish forking the repository

Step 2:
Open a command line and create a new directory to house all of the gitian building and then cd into it. This can be
wherever you'd like but I placed it in my home directory, something like:
```
cd ~
mkdir gitian
cd gitian
```

enter below, where "githubusername" is your github username and "0.16.0.0" is the version you are building for
```
export NAME=githubusername
export VERSION=0.16.0.0
```

Then, you need to get the gitian-build.py script.
```
wget https://raw.githubusercontent.com/dashpay/dash/v${VERSION}/contrib/gitian-build.py
chmod +x gitian-build.py
```

At this point if you don't have python3 and git installed, install it
```
sudo apt install python3 git
```

Clone your gitian.sigs repo
```
git clone https://github.com/$NAME/gitian.sigs
```

Now, run gitian-build.py script in setup mode. You may get asked to enter your password. 
```
./gitian-build.py -V lxc --setup $NAME $VERSION
```

You now need to reboot your computer.

Now that everything is setup, you can build it. You may get asked to enter your password.

This will take quite a while! You can add `-j X` for X threads, and `-m XXXX` for XXXX MB of RAM usage
```
./gitian-build.py -V lxc -B $NAME $VERSION
```

In order to create a new pull request run below and then click on the link / navigate to the address as given in the console
```
git push origin $VERSION
echo "https://github.com/dashpay/gitian.sigs/compare/master...$NAME:$VERSION?expand=1"
```
