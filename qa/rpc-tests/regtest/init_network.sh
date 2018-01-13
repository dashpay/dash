#!/bin/bash

# initialize dash regression test network with specified number of masternodes and regular nodes

tmpdir=$1
srcdir=$2

mncount=7
rncount=3
nodecount=$(($mncount+$rncount))

for (( i=1; i<=$nodecount; i++ ))
do
    echo "stopping dashd daemon $i"
    if [ -f $tmpdir/$i/regtest/dashd.pid ]
    then
        pid=`cat $tmpdir/$i/regtest/dashd.pid`
        $srcdir/dash-cli -datadir=$tmpdir/$i stop &>/dev/null
        while [ -e /proc/$pid ]; do sleep 0.1; done
    fi
done

for (( i=1; i<=$nodecount; i++ ))
do
    rm -rf $tmpdir/$i
    mkdir $tmpdir/$i

    echo "regtest=1" > $tmpdir/$i/dash.conf

    for (( j=1; j<=$nodecount; j++ ))
    do
        if [ $j -ne $i ]
        then
            echo "connect=127.0.0.$j" >> $tmpdir/$i/dash.conf
        fi
    done
    
    echo "listen=1" >> $tmpdir/$i/dash.conf
    echo "bind=127.0.0.$i:19994" >> $tmpdir/$i/dash.conf
    port=$((30000+$i))
    echo "rpcport=$port" >> $tmpdir/$i/dash.conf

    echo "rpcuser=user" >> $tmpdir/$i/dash.conf
    echo "rpcpassword=pass" >> $tmpdir/$i/dash.conf

    mkdir -p $tmpdir/$i/regtest
    echo "" >> $tmpdir/$i/regtest/debug.log
    
    if [ $i -eq $nodecount ]
    then
        $srcdir/dashd -keypool=1 -datadir=$tmpdir/$i -govtest -debug -testlog=/tmp/node$i -daemon
    else
        $srcdir/dashd -keypool=1 -datadir=$tmpdir/$i -debug -testlog=/tmp/node$i -daemon
    fi
    ( tail -f -n0 $tmpdir/$i/regtest/debug.log & ) | grep -q "Done loading"
done


echo "generating blocks"
$srcdir/dash-cli -datadir=1 generate $((100+$nodecount*2+1)) &>/dev/null

for (( i=1; i<=$nodecount; i++ ))
do
    address=`$srcdir/dash-cli -datadir=$tmpdir/$i getaccountaddress 0`
    txid[$i]=`$srcdir/dash-cli -datadir=1 sendtoaddress $address 1000`

    if [ $i -gt $mncount ]
    then
        echo "configuring node $i"
    else
        echo "configuring masternode $i"

        mnkey[$i]=`$srcdir/dash-cli -datadir=$tmpdir/$i masternode genkey`
        echo "masternode=1" >> $tmpdir/$i/dash.conf
        echo "masternodeprivkey=${mnkey[$i]}" >> $tmpdir/$i/dash.conf

        echo "mn$i 127.0.0.$i:19994 ${mnkey[$i]} ${txid[$i]} 1" >> $tmpdir/$i/regtest/masternode.conf
    fi
done


$srcdir/dash-cli -datadir=1 generate 15 &>/dev/null


for (( i=1; i<=$mncount; i++ ))
do
    echo "restarting masternode $i"
    $srcdir/dash-cli -datadir=$tmpdir/$i stop &>/dev/null
    pid=`cat $tmpdir/$i/regtest/dashd.pid`
    while [ -e /proc/$pid ]; do sleep 0.1; done
done

sleep 5

for (( i=1; i<=$mncount; i++ ))
do
    if [ $i -eq 1 ]
    then
        $srcdir/dashd -keypool=1 -datadir=$tmpdir/$i -govtest -debug -testlog=/tmp/node$i -daemon
    else
        $srcdir/dashd -keypool=1 -datadir=$tmpdir/$i -debug -testlog=/tmp/node$i -daemon
    fi
    
    ( tail -f -n0 $tmpdir/$i/regtest/debug.log & ) | grep -q "Done loading"
    #$srcdir/dash-cli -datadir=$tmpdir/$i masternode start
done
