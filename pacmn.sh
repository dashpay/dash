#!/bin/bash

set -e

if [ "$1" == "--testnet" ]; then
	pac_rpc_port=17111
	pac_port=17112
	is_testnet=1
else
	pac_rpc_port=7111
	pac_port=7112
	is_testnet=0
fi

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

echo "################################################"
echo "#   Welcome to PAC Masternode's server setup   #"		
echo "################################################"
echo "" 
read -p 'Please provide the external IP: ' ipaddr
read -p 'Please provide masternode genkey: ' mnkey

while [[ $ipaddr = '' ]] || [[ $ipaddr = ' ' ]]; do
	read -p 'You did not provided an external IP, please provide one: ' ipaddr
	sleep 2
done

while [[ $mnkey = '' ]] || [[ $mnkey = ' ' ]]; do
	read -p 'You did not provided masternode genkey, please provide one: ' mnkey
	sleep 2
done

echo "###############################"
echo "#  Installing Dependencies    #"		
echo "###############################"
echo ""
echo "Running this script on Ubuntu 16.04 LTS or newer is highly recommended."

sudo apt-get -y update
sudo apt-get -y install git python virtualenv ufw pwgen 


echo "###############################"
echo "#   Setting up the Firewall   #"		
echo "###############################"
sudo ufw status
sudo ufw disable
sudo ufw allow ssh/tcp
sudo ufw limit ssh/tcp
sudo ufw allow $pac_port/tcp
sudo ufw logging on
sudo ufw --force enable
sudo ufw status

sudo iptables -A INPUT -p tcp --dport $pac_port -j ACCEPT

echo ""
echo "###############################"
echo "#      Get/Setup binaries     #"		
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
	rm -r $tarball_name
else
	echo "There was a problem downloading the binaries, please try running again the script."
	exit -1
fi

echo "###############################"
echo "#     Configure the wallet    #"		
echo "###############################"
echo ""
echo "The .paccoincore folder will be created, if folder already exists, it will be replaced"
if [ -d ~/.paccoincore ]; then
	if [ -e ~/.paccoincore/paccoin.conf ]; then
		read -p "The file paccoin.conf already exists and will be replaced. do you agree [y/n]:" cont
		if [ $cont = 'y' ] || [ $cont = 'yes' ] || [ $cont = 'Y' ] || [ $cont = 'Yes' ]; then
			sudo rm ~/.paccoincore/paccoin.conf
			touch ~/.paccoincore/paccoin.conf
			cd ~/.paccoincore
		fi
	fi
else
	echo "Creating .paccoincore dir"
	mkdir -p ~/.paccoincore
	cd ~/.paccoincore
	touch paccoin.conf
fi

echo "Configuring the paccoin.conf"
echo "rpcuser=$(pwgen -s 16 1)" > paccoin.conf
echo "rpcpassword=$(pwgen -s 64 1)" >> paccoin.conf
echo "rpcallowip=127.0.0.1" >> paccoin.conf
echo "rpcport=$pac_rpc_port" >> paccoin.conf
echo "externalip=$ipaddr" >> paccoin.conf
echo "port=$pac_port" >> paccoin.conf
echo "server=1" >> paccoin.conf
echo "daemon=1" >> paccoin.conf
echo "listen=1" >> paccoin.conf
echo "testnet=$is_testnet" >> paccoin.conf
echo "masternode=1" >> paccoin.conf
echo "masternodeaddr=$ipaddr:$pac_port" >> paccoin.conf
echo "masternodeprivkey=$mnkey" >> paccoin.conf


echo "###############################"
echo "#      Running the wallet     #"		
echo "###############################"
echo ""
cd ~/

./paccoind
sleep 60

is_pac_running=`ps ax | grep -v grep | grep paccoind | wc -l`
if [ $is_pac_running -eq 0 ]; then
	echo "The daemon is not running or there is an issue, please restart the daemon!"
	exit
fi

echo "###############################"
echo "#     Running the sentinel    #"		
echo "###############################"
echo ""
git clone "https://github.com/PACCommunity/sentinel"
cd sentinel
virtualenv ./venv
./venv/bin/pip install -r requirements.txt
venv/bin/python bin/sentinel.py
sleep 3
crontab 'crontab.txt'

cd ~/
./paccoin-cli getinfo

echo "Your masternode server is ready!"
echo "Don't forget to run the masternode from your cold wallet!"