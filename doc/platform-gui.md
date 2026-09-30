# Dash Platform in dash-qt

`--enable-platform-gui` adds DashPay (usernames, profiles, contacts and
payments by username) to `dash-qt`. It is off by default, needs the GUI and
the wallet, and links the Platform-owned `dash-platform-cxx` archive (a thin
C++ shell over the Rust `dash-sdk`) into `dash-qt` (and its multiprocess
twin `dash-gui`), `test_dash` and `test_dash-qt` only. `dashd`, `dash-cli`, `dash-tx`, `dash-wallet` and the
fuzz binary never link it; a default build is byte-identical with or without
the option.

Two layers implement it:

- `src/platform/` (`libdash_platform.a`, Qt-free): the `PlatformClient` seam,
  its SDK-backed implementation, the signing operation and wallet signer, the
  state-transition adapters and the wallet record formats.
- `src/qt/platform/`: the per-wallet service, the identity, contact and
  recovery state machines, and the pages and dialogs.

## Trust model

Every response byte comes from an untrusted evonode. Nothing in the linked
archive fetches from a trusted third-party service: there is no HTTP client,
no default seed list and no remote quorum source. Trust rests on inputs Core
pushes into the SDK:

- **Endpoints** come from the valid entries of the deterministic masternode
  list at the chain tip (`getPlatformHTTPSAddrs`), refreshed every minute. An
  empty set removes every endpoint.
- **Quorum keys** are the public keys of the mined final commitments of the
  network's Platform LLMQ type (`Consensus::Params::llmqTypePlatform`), in
  Core's internal byte order; the shell normalizes them. The SDK refuses a
  proof signed by any other LLMQ type or by a quorum Core has not pushed.
- **ChainLock height** is the best local ChainLock, pushed on the timer and
  on every `NotifyChainLock`. No proved read is dispatched before the first
  push, and a proof whose signed core-chain-locked height trails the local
  one by more than 288 blocks is refused as stale. There is no ceiling: an
  evonode one ChainLock ahead of this node is honest.
- **Chain id** is `CChainParams::PlatformChainId()`, overridable through the
  GUI-only `-platformchainid` on every network except mainnet.

Every read is proved: the SDK replays the GroveDB proof and verifies the
Tenderdash quorum signature before anything reaches the GUI, and absence is
a proven outcome (`StatusKind::PROVEN_ABSENT`), never inferred from a
failure. The broadcast reply is advisory; every write is confirmed by a
proved re-query of the object it created.

### Freshness order

For every proved read, in order: (1) the quorum signature, with the LLMQ
type, quorum set and ChainLock lag gates above; (2) the SDK's signed-time
window (10 minutes) and its protocol-version ratchet; (3) the shell's
chain-id check (`CHAIN_ID_MISMATCH`); (4) the shell's monotonic
Platform-height watermark (tolerance 3 blocks, `REJECTED`); (5) a protocol
version above what the build knows (`UNSUPPORTED_PROTOCOL_VERSION`): the
value is still returned, the GUI freezes writes and asks for an update.

### Protocol version policy

The SDK builds state transitions under the protocol version a verified read
has shown the network to run, and refuses to build before the first such
read. Each Platform protocol version bump therefore needs a Dash Core point
release that repins `dash-platform-cxx`; until then reads keep working and
writes are frozen.

## Custody contract

Private keys, the seed and the mnemonic never cross into Rust. The SDK
builders hand the wallet the full signable preimage of a transition
(`WalletSigner::signForKey`); Core computes the double SHA256 itself, checks
that the first byte (the bincode variant index of `StateTransition`: 2 for a
batch, 3 for an identity create) matches the operation it is in, refuses any
key outside the operation, and answers with the wallet's 65-byte compact
recoverable signature through `interfaces::Wallet::signPlatformDigest`. The
asset-lock sighash of an identity registration is the one digest path
(`signAssetLockSighash`), accepted once per operation for the flow's funding
key. The ECDH secret and the accountReference MAC of a contact request are
computed inside `CWallet` (`platformECDHSecret`,
`platformAccountReferenceMac`, both refusing the MASTER key) and only their
32-byte outputs are handed to the SDK, which zeroizes its copies.

A builder can only be called with a `platform::SigningOperation`: move-only,
minted by `PlatformService` alone, carrying the operation kind, the key ids
it may sign with, the one-shot asset-lock flag and the wallet unlock scope.
That scope is released before any network wait, so an encrypted wallet is
unlocked only for the milliseconds a step signs. A locked wallet never signs;
the identity flow parks in `NEEDS_UNLOCK` and retries on the next user
action.

## Threading

- `SdkClient` runs every read and broadcast on one serial worker thread; one
  enqueue is one SDK request (one page, one broadcast, one fetch), so the
  worst-case head-of-line delay is one request budget (20 s, a 5 s connect or
  15 s through a proxy, 2 retries). Paging is a flow concern: a flow
  re-enqueues with the cursor of the previous page on a later tick.
- Callbacks fire on the worker; the Qt layer re-posts them to the GUI thread.
  The GUI never waits on the worker while holding `cs_wallet`.
- Builders run to completion on the calling (GUI) thread with no network
  access; the SDK drives its async builders with a local executor, so the
  `WalletSigner` is only invoked on the thread that called the builder. The
  signer is nonetheless callable from any thread: it wraps wallet seams that
  take `cs_wallet` and keeps no thread-local state.
- `shutdown()` aborts the in-flight request, stops the SDK runtime and joins
  the worker before the flows are destroyed.

## Privacy

The service is created only for a wallet whose owner enabled DashPay in the
opt-in dialog (record `platform/enabled`), which states what the evonode
answering each request can see: the node's IP address, the wallet's identity
and what it looks up, and the usernames searched for. DashPay is turned on and
off per wallet in Options, Wallet. The service needs a descriptor wallet, a
synced node and a local ChainLock, and pushes an empty endpoint set while the
network is inactive or the node is still in initial block download. No avatar
is ever fetched or rendered.

Platform connections follow the node's own network settings. When IPv4 or
IPv6 is reachable, every connection goes through `-proxy`, or directly when
none is set, and only evonodes on a reachable network are used; an onion
evonode only when the onion proxy is that same proxy. With only onion
reachable (`-onlynet=onion`), connections go through the onion proxy to onion
evonodes only. I2P and CJDNS evonodes are never used. The client's proxy is
fixed when the service is created, as the node's is at startup, and with
`-proxyrandomize` every connection gets fresh SOCKS5 credentials, so Tor
builds a circuit per connection. Evonode endpoints are IP addresses or onion
names, so nothing is ever resolved locally, and TLS is verified end to end
through the proxy. An onion evonode is therefore only usable with a
certificate valid for its onion name, which few certificate authorities
issue, so an onion-only node usually reaches no evonode and DashPay reports
Dash Platform as unavailable. A proxy that fails does not count against the
evonode. When no network DashPay can use is reachable the service is not
created, and when no evonode can be reached over the networks that are, it
pushes no endpoints; the page says why in either case.

Every proved read discloses the identity it concerns to the evonode that
answers it, so the GUI reads only while the DashPay page is shown, never for a
hidden page: the dashboard's profile and balance when the page is shown or the
window becomes active (not again within 30 seconds); the contact list also on
a new ChainLock (at most once a minute) and on a five-minute fallback, backing
off to ten minutes while reads fail; and whatever the user's own change
touched. There is no manual refresh; a failed read offers Try again. A
contact's username and profile are re-read at most once an hour; username
search results are kept in memory for the session and never written to the
wallet; and a recipient typed into the send form is looked up only once it can
no longer be the start of a Dash address, or when the entry is left.

Debug logging of the library and the GUI layer uses the `platform` category
(`-debug=platform`).

## Wallet records

Records live in the wallet database under `platform/*`, `identity/*` and
`contact/*` and travel with backups. They are written for one chain
(`platform/chain-id`, stamped by the first response verified for it, never
by configuration alone) and one layout version (`platform/version`); when
either differs from what the build expects, every Platform record is wiped
and seed-only recovery reruns from the on-chain state. There is no
migration path.

The identity record names the ids of the keys this wallet signs documents
with and runs the contact-request ECDH with. A registration sets 1, 2 and 3;
seed-only recovery takes them from the proved identity and only restores an
identity whose keys at those ids are the ones the wallet derives there, so
an identity registered by another wallet with a different key layout is
reported instead of surfacing as one that can never sign. A
`platform/recovery-pending` record marks a restored identity whose contacts
were not all restored yet; the next start resumes that phase. No new
identity registration starts before recovery has proved that the seed has
none, since Platform refuses a second identity with the same keys and the
asset lock funding it would be burned.

A failed registration keeps what it put on chain: "try again" re-uses an
unconsumed asset lock, or asks for a new name paid from the balance on Dash
Platform of an identity that already exists. Unanswered or unverified reads never fail a
registration; the step is retried on the next tick.

The rescan birth time of an imported friendship keychain is the time of our
own outgoing contact request, never the counterparty's document time, which
the sender controls.

A request that answers one we sent establishes the contact on the next
refresh without broadcasting anything, as the mobile wallets do; a locked
wallet shows the contact as accepted until it is unlocked.

A contact request can be neither rejected nor withdrawn on Platform, so
"Ignore request" and "Hide contact" only write a `contact/hidden/<id>`
record in this wallet: the other side is never told, the record is not
restored by seed recovery, and sending that person a request clears it.

## Known build limitations

- Stable `rustc` emits no CET/IBT or BTI instrumentation, so a Platform
  enabled `dash-qt` loses that hardening in the Rust closure.
- Windows builds link `crypt32`, `ntdll`, `secur32` and `ncrypt` for the Rust
  TLS trust store and standard library.
