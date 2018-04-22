#!/bin/bash

set -e
cd ~/

arch=`uname -m`
version="0.12.3.0"
old_version="0.12.2.3"
base_url="https://github.com/PACCommunity/PAC/releases/download/v${version}"
if [ "${arch}" == "x86_64" ]; then
	tarball_name="PAC-v${version}-linux-x86_64.tar.gz"
	binary_url="${base_url}/${tarball_name}"
elif [ "${arch}" == "x86_32" ]; then
	tarball_name="PAC-v${version}-linux-x86.tar.gz"
	binary_url="${base_url}/${tarball_name}"
else
	echo "PAC binary distribution not available for the architecture: ${arch}"
	exit -1
fi

echo "        ..........    												 "
echo "      .............. 												     "
echo "   ....................   											 "
echo "  .....-yyyyyyys+-:oo-..       oooooooo.      ooooo         oooooooo   "
echo " ......:MMMMNNdddmMMMd...     MMMMNMMMMMh.   MMMMMMM      MMMMMMMMMM   "
echo "......:MMmy..-+MNdh:.....     MMMM: .dMMMy  oMMMyMMNN   .NMMMMy        "
echo "......:ddmm...-hdNm......     MMMM+-+mMMMs :MMMs dMMm   yMMMMo         "
echo "....:ohNMMNdoydNMMN/.....     MMMMMMMMMmo. NMMN  -MMMd  yMMMM.         "
echo "...+dMNdddNmmdhy+-.......     MMMM+::-    dMMMNmmmMMMMs :MMMMm.        "
echo "......::MMMm.............     MMMM:      yMMMdoooooNMMM  mMMMMNdddN+   "
echo " ......:MNdo.............     dddd-     ddddd.     ddddd.  :ohdmmdhy:  "
echo "  ....-+-..............                                                "
echo "    .................                                                  "
echo "       ...........  													 "

echo "################################################"
echo "#                   Welcome   	             #"
echo "################################################"
echo ""
echo "This script will update PAC to the latest version (${version})."
echo "Before running the script we recommend to backup your private keys and your wallet.dat file that is under ~/.paccoincore/wallet.dat even though this script will not affect them"
read -p "Do you want to auto-backup wallet.dat and continue with the process? [y/n]: " cont
if [ $cont = 'n' ] || [ $cont = 'no' ] || [ $cont = 'N' ] || [ $cont = 'No' ]; then
	exit -1
fi

if [ -e ~/paccoin-cli ] && [ -e ~/paccoind ]; then
	echo ""
	echo "#######################################"
	echo "#   Stopping pac services (daemon)    #"
	echo "#######################################"
	echo ""

	wallstatus=$( ./paccoin-cli mnsync status ) || true
	if [ -z "$wallstatus" ]; then
		echo "Daemon is not running no need to stop it"
	else
		echo "Stopping the Daemon"
		./paccoin-cli stop
		sleep 60

		wallstatus=$( ./paccoin-cli mnsync status ) || true
		if [ -z "$wallstatus" ]; then
			echo "Daemon stopped!"
		else
			echo "Daemon was not stopped! Please kill the deamon process or run ./paccoin-cli stop, then run the script again"
			exit
		fi
	fi

	echo ""
	echo "###############################"
	echo "#   Removing old binaries    #"
	echo "###############################"
	echo ""

	if [ -e paccoin-cli ]; then
		echo "Removing paccoin-cli"
		rm paccoin-cli
	fi

	if [ -e paccoind ]; then
		echo "Removing paccoind"
		rm paccoind
	fi

	if [ -e paccoin-qt ]; then
		echo "Removing paccoin-qt"
		rm paccoin-qt
	fi

	if [ -e ~/PAC-v0.12.2.3-ubuntu-16.04-x64.tar.gz ]; then
		echo "Removing PAC-v0.12.2.3-ubuntu-16.04-x64.tar.gz"
		rm PAC-v0.12.2.3-ubuntu-16.04-x64.tar.gz
	fi

	if [ -d ~/PAC-v0.12.2.3-ubuntu-16.04-x64 ]; then
		echo "Removing PAC-v0.12.2.3-ubuntu-16.04-x64 directory"
		rm -r PAC-v0.12.2.3-ubuntu-16.04-x64
	fi
else
	is_pac_running=`ps ax | grep -v grep | grep paccoind | wc -l`
	if [ $is_pac_running -gt 0 ]; then
		echo "PAC process is still running, it's not safe to continue with the update, exiting."
		echo "Please stop the daemon with './pacoin-cli stop' or kill the daeomon process, then run the script again."
		exit -1
	fi
fi

echo ""
echo "#######################################"
echo "#      Backing up the wallet.dat      #"		
echo "#######################################"
echo ""
is_pac_running=`ps ax | grep -v grep | grep paccoind | wc -l`
if [ $is_pac_running -gt 0 ]; then
	echo "PAC process is still running, it's not safe to continue with the update, exiting."
	echo "Please stop the daemon with './paccoin-cli stop' or kill the daemon process, then run the script again."
	exit -1
else
	currpath=$( pwd )
	echo "Backing up the wallet.dat"
	backupsdir="pac_wallet_backups"
	mkdir -p $backupsdir
	backupfilename=wallet.dat.$(date +%F_%T)
	cp ~/.paccoincore/wallet.dat "$currpath/$backupsdir/$backupfilename"
	echo "wallet.dat was saved to : $currpath/$backupsdir/$backupfilename"
fi

echo ""
echo "###############################"
echo "#   Get/Setup new binaries    #"
echo "###############################"
echo ""

if test -e "${tarball_name}"; then
	rm -r $tarball_name
fi
wget $binary_url
if test -e "${tarball_name}"; then
	echo "Unpacking $PAC distribution"
	tar -xvzf $tarball_name
	chmod +x paccoind
	chmod +x paccoin-cli
	echo "Binaries were saved to: $PWD/$tarball_name"
else
	echo "There was a problem downloading the binaries, please try running again the script."
	exit -1
fi

echo ""
echo "##################################################################"
echo "#   Updating the sentinel (Applies only on masternodes)       #"
echo "##################################################################"
echo ""

was_sentinel_found=0
if [ -d ~/sentinel ]; then
	was_sentinel_found=1
	cd sentinel
	git pull
	cd ..
fi

echo ""
echo "###########################"
echo "#   Running the daemon    #"
echo "###########################"
echo ""

cd ~/
./paccoind -daemon=1
echo "Waiting for daemon to be up and running"
sleep 60
./paccoin-cli getinfo
echo "PAC Updated!"
echo "Remember to go to your cold wallet and start the masternode (cold wallet must also be on the latest version)."

if [ $was_sentinel_found -eq 0 ]; then
	echo "No sentinel was found, in case this is a masternode follow the instructions below. If this is not a masternode, then ignore this message."
	echo "Intructions:"
	echo "1) Go to the sentinel folder"
	echo "2) Run this command: git pull"
fi