Updated RPCs
------------

- `masternode connect` opens a masternode connection again. Since v18.1.0 it opened a masternode probe instead, which never sent our MNAUTH and was dropped as soon as the peer's MNAUTH arrived, after the RPC had already reported success. When called on a masternode, the connection is now authenticated by both ends. As with other masternode connections, it is closed once the node has enough non-masternode peers, unless the peer is one of its quorum or quorum relay peers. (#8051)
