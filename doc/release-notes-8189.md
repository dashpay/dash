P2P and network changes
-----------------------

- Network-adjusted time has been removed. Governance, sporks, CoinJoin and LLMQ
  status now use the system clock, as block validation already did, so the
  system clock has to be kept accurate (for example with NTP). This matters for
  masternodes and for wallets that mix with CoinJoin: CoinJoin queues are only
  accepted within 30 seconds of the receiving node's clock, while the
  out-of-sync warning below only fires above 10 minutes.
- The `-maxtimeadjustment` option has been removed. Passing it on the command
  line is now an error; a leftover entry in `dash.conf` is ignored with a log
  message.
- The time offsets of outbound peers are now only used to warn that the local
  clock appears to be out of sync. Once at least 5 outbound peers have
  connected, the warning is raised when the median offset of the last 50
  outbound peers exceeds 10 minutes, and `getnetworkinfo`'s `timeoffset`
  reports that median.
