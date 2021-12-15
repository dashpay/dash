#!/bin/bash

export LC_ALL=C
set -e

# Get Tor service IP if running
if [[ "$1" == "dashd" ]]; then
  # Because dashd only accept torcontrol= host as an ip only, we resolve it here and add to config
  if [[ "$TOR_CONTROL_HOST" ]] && [[ "$TOR_CONTROL_PORT" ]] && [[ "$TOR_PROXY_PORT" ]]; then
    TOR_IP=$(getent hosts $TOR_CONTROL_HOST | cut -d ' ' -f 1)
    echo "proxy=$TOR_IP:$TOR_PROXY_PORT" >> "$HOME/.dashcore/dash.conf"
    echo "Added "proxy=$TOR_IP:$TOR_PROXY_PORT" to $HOME/.dashcore/dash.conf"
    echo "torcontrol=$TOR_IP:$TOR_CONTROL_PORT" >> "$HOME/.dashcore/dash.conf"
    echo "Added "torcontrol=$TOR_IP:$TOR_CONTROL_PORT" to $HOME/.dashcore/dash.conf"
    echo -e "\n"
  else
    echo "Tor control credentials not provided"
  fi
  
  # If an masternode private key is provided (length of $CORE_MASTERNODE_OPERATOR_PRIVATE_KEY is nonzero) then dashd should be run as a masternode. Then we add masternodeblsprivkey to config.
  if [[ -z $CORE_MASTERNODE_OPERATOR_PRIVATE_KEY ]]; then
    echo "No CORE_MASTERNODE_OPERATOR_PRIVATE_KEY provided, this node is a regular wallet node."
  else
    echo "masternodeblsprivkey=$CORE_MASTERNODE_OPERATOR_PRIVATE_KEY" >> "$HOME/.dashcore/dash.conf"
    echo "Added "masternodeblsprivkey=$CORE_MASTERNODE_OPERATOR_PRIVATE_KEY" to $HOME/.dashcore/dash.conf"
  fi
fi

exec "$@"
