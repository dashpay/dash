# Taproot on Dash Core: working notes for `bp-taproot`

Agent notes. This file does not belong on the final branch; drop the commit
before opening the PR.

## Goal and scope

A working mockup of BIP 340/341/342 on Dash without any SegWit structure:
taproot addresses, `tr()` / `rawtr()` / `multi_a()` descriptors, raw-key and
wallet signing (key path and script path), PSBT taproot fields, Qt address
type dropdown. No witness serialization, no "fake scripts", no DIP0143/BIP143.

Upstream PR candidates are listed in `bp-taproot.txt` (repo root). Backports
are done only with `git cherry-pick -m1 <merge>`; the upstream merge objects
are reachable via the `bitcoin` remote, and `~/bitcoin` is a checkout for
reference.

## Design decisions (confirmed by the user)

- **Spend encoding.** The BIP 341 "witness stack" is the sequence of pushes in
  the input `scriptSig`. `VerifyScript` dispatches on
  `CScript::IsPayToTaproot()` (`OP_1 <32 bytes>`) before any legacy
  evaluation: `ParseTaprootStack` -> `VerifyTaprootProgram` ->
  `ExecuteTapscript`. Push-only, no 520-byte element cap, no MINIMALDATA on
  the stack items. A node without `SCRIPT_VERIFY_TAPROOT` evaluates the
  output as a plain script and rejects pushes over 520 bytes, so activation
  is a hard fork for it. Activation gate is `DEPLOYMENT_TAPROOT` (bit 15,
  EHF, own deployment since 2026-11-10 because v24 had already shipped).
- **Sighash.** `SignatureHashSchnorr` writes `int32(nVersion | nType << 16)`,
  `nLockTime`, and for `nVersion >= SPECIAL_VERSION && nType !=
  TRANSACTION_NORMAL` the serialized `vExtraPayload`, exactly like the legacy
  `SignatureHash`. Python `TaprootSignatureMsg` mirrors it (length assert
  includes `payload_size`). Vectors pinned in
  `script_tests/bip341_dash_special_tx_sighash`.
- **OP_SUCCESSx.** BIP 342 set minus the DIP0020 opcodes (`OP_CAT`,
  `OP_SPLIT`, `OP_NUM2BIN`, `OP_BIN2NUM`, `OP_AND`, `OP_OR`, `OP_XOR`,
  `OP_DIV`, `OP_MOD`, `OP_CHECKDATASIG(VERIFY)`) and minus `OP_CHECKSIGADD`,
  which is `0xbc` in Dash (0xba/0xbb are CHECKDATASIG). `MAX_OPCODE` stays
  `OP_CHECKDATASIGVERIFY`, like upstream keeps it at `OP_NOP10`: asm parsing
  of `OP_CHECKSIGADD` is an error (`script_parse_tests` checks this), and
  `decodescript` names the opcode explicitly in its wrap check.
- **Limits.** `MAX_SCRIPT_SIZE` (10 kB consensus), `MAX_STANDARD_SCRIPTSIG_SIZE`
  (1650 B policy) and the DIP0001 100 kB tx size all apply to taproot spends.
  User: these would be lifted for BIP143, not for taproot. Consequences:
  `feature_taproot` big multisig is 800-of-800 (`BIG_MULTISIG_N`), the
  wallet `multi_a` case uses `MAX_STANDARD_MULTI_A_KEYS = 40`.
- **Policy.** `IsTaprootStandard` (no annex; tapscript stack items <= 80 B)
  wired in `MemPoolAccept::PreChecks` after `AreInputsStandard(..., taproot_active)`
  with reason `bad-taproot-nonstandard`. DISCOURAGE_* flags are in
  `STANDARD_SCRIPT_VERIFY_FLAGS`.
- **Addresses.** bech32m with a dedicated L1 HRP in `CChainParams::bech32_hrp`
  / `Bech32HRP()`: `dc` mainnet, `tdc` testnet, devnet and regtest (two HRPs,
  same as Platform's `dash`/`tdash`). Decoding branch sits before the Platform
  branch in `key_io.cpp`. Python: `output_key_to_p2tr(key, main=False)`.
- **Destination type.** `TaprootDestination : XOnlyPubKey` (renamed from
  upstream `WitnessV1Taproot`, own refactor commit). `getaddressinfo` /
  `validateaddress` report `isscript: true` and `output_key`.
- **Output type.** `OutputType::TAPROOT` ("taproot"). Wallet keeps one active
  `ScriptPubKeyMan` per `OutputType` (`std::map<OutputType, ScriptPubKeyMan*>`
  for external and internal). On disk LEGACY keeps the bare
  `activeexternalspk` / `activeinternalspk` record; other types append a
  type byte (`std::make_pair(key, type)`); the loader reads the trailing byte
  if present. `getnewaddress` / `getrawchangeaddress` take `address_type`
  ("legacy" default via `CWallet::m_default_address_type`, or "taproot").
  Change type: legacy if the wallet can produce it, else the first type with
  an internal manager (`TransactionChangeType` in `wallet/spend.cpp`).
- **Signing.** Dummy Schnorr signature is 65 bytes (wallet signs
  `SIGHASH_ALL`; no `SIGHASH_DEFAULT` RPC default switch). `tr()` import is
  gated on `isTaprootActive()`.
- **Qt.** Receive tab dropdown "Legacy" / "Taproot", the latter only when the
  descriptor wallet has an active TAPROOT external manager
  (`interfaces::Wallet::taprootEnabled()`).

## Known gaps (document in the PR, not bugs to fix now)

- Fee estimation for script-path spends assumes the key path whenever the
  internal key is known to the signing provider, even without its private
  key (e.g. a watch-only `tr(XPUB, multi_a(...))`); tests use higher fee
  rates there. Outputs with the NUMS internal key H are sized correctly
  since the dummy signer refuses a key path for H and unknown keys.
- Address index does not index P2TR. `CAddressIndexKey` and friends store a
  `uint160`; a 32-byte output key needs a new key layout (or a uint256
  field) and therefore a reindex for `-addressindex` users. Decision pending.
- `MAX_STANDARD_SCRIPTSIG_SIZE` bounds tapscript spends; raising it for
  taproot inputs is a follow-up.
- Miniscript in tapscript: Dash has no miniscript descriptors at all
  (bitcoin#24148 was reverted because it was wsh-only, #24149 is partial).
  Bringing it needs #24148 adapted to tr()-only, the rest of #24149, then
  #27255 (MiniTapscript, ~1.4k lines; prerequisites #28244, #26567, #27997,
  #28067, #28587 are already in Dash). Decision pending.
- Not taken: upstream `gen_test_vectors`, `SIGHASH_DEFAULT` RPC defaults
  (#22514), MiniWallet P2TR (#23371).

## Done after the first review round (2026-10-20)

- bitcoin#22364: descriptor wallets created where taproot is active derive
  `tr(xpub/86h/coin'h/0h/{0,1}/*)` next to `pkh()` (BIP86 purpose,
  `BIP32_PURPOSE_TAPROOT` in `hdchain.h`); no tr() for CoinJoin. Gated on
  `chain().isTaprootActive()` like the import path, so wallets created earlier
  need `importdescriptors`.
- `-addresstype` (legacy|taproot) and `-changetype`; `DEFAULT_ADDRESS_TYPE`
  in `wallet.h`.
- bitcoin#23578: external signers may return tr() descriptors; activated
  when taproot is active (`IsDefaultPurposeDescriptor` accepts 44' and 86').
- bitcoin#23789: change type follows the payment outputs
  (`TransactionChangeType` in `wallet/spend.cpp`, static). Dash difference:
  the "legacy default means legacy change" shortcut only applies when the
  wallet has a legacy internal manager, so tr()-only wallets keep working.
- bitcoin#30048: `XOnlyPubKey::NUMS_H`; Dash commit on top fixes the dummy
  signer for fee estimation.
- `wallet_taproot.py` covers all of the above; `wallet_signer.py` activates
  taproot through `-vbparams` on both nodes.

## Test activation without masternodes

Active node: `-vbparams=taproot:0:999999999999:0:10:8:6:5:0` (trailing `0` means
`useEHF=false`), then mine through the window. Inactive node:
`-vbparams=taproot:1:1`; it cannot follow an active chain (hard fork), so
`feature_taproot` lets it mine its own coins. All wallet tests run as
`--descriptors` (no BDB on this machine).

## Branch state (2026-11-10, after the rebase onto develop)

Rebased onto `origin/develop` at 6271b740e9 (dash#8067). 62 commits plus two
temporary ones on top (dash#8080, the `test_dash-qt` fix for
`SharedMnWizardTests::signedSessionReservation`; drop them together with the
notes commit once #8080 is merged). No `fixup!` commits remain: every fixup
was squashed into its target, the early "disable ALL spenders"/revert pair
was dropped, and the two notes commits became one.

How the rebase was done (two passes, so each could be verified):

1. Squash on the old base, then `git diff` against the old head: empty.
2. Rebase onto develop. Conflicts, all in tests: `address.py` imports,
   `descriptor_tests.cpp` flag constants (develop added `MIXED_PUBKEYS = 32`
   from bitcoin#23171; `XONLY_KEYS = 64` and `MISSING_PRIVKEYS = 128` kept as
   upstream), `wallet_signer.py` extra_args, `crypto/secp256k1.py` (develop's
   `lru_cache` plus the new `test_H`), `rpc_decodescript.json` HRP.
   One code adaptation: `CKey::SignSchnorr` passes `UCharCast(begin())`
   because develop moved `CKey` to `std::byte` storage (bitcoin#29133);
   squashed into the bitcoin#21365 commit. Second: the generated
   `*.json.h` test headers are `std::string` now (bitcoin#28168), so the two
   `bip341_wallet_vectors` readers use upstream's `Assert(tests.read(...))`
   form (`read_json()` does not fit, the vectors are an object, not an
   array); squashed into the bitcoin#23394 commit. Stale generated headers in a build tree must be
   deleted by hand (`find src -name '*.json.h' -delete`), the make rule does
   not depend on the Makefile.

Backup refs in this checkout: `bp-taproot-pre-rebase` (old 91-commit head),
`bp-taproot-squashed` (pass 1 on the old base), `bp-taproot-on-4c5c0153`
(first rebase target before develop moved).

Verification on the old develop tip (4c5c015350): build with zero warnings,
lint, `test_dash` 172 suites, and 29 functional tests (feature_taproot,
wallet_taproot, wallet_signer, rpc_psbt, rpc_decodescript, feature_block,
the descriptor-wallet set). `test_dash-qt` crashed in
`SharedMnWizardTests::signedSessionReservation` with heap corruption; that is
develop's issue dash#8075 (stale message-box timers from `AddressBookTests`
under the GLib event dispatcher), fixed by dash#8080, which is why its two
commits ride on top for now. Results on the current tip: see the end of the
file.

This checkout had been configured for a TSAN benchmark build (the CI
`linux64_tsan` line) when the rebase started. It is back on the regular
debug configuration; the TSAN line is in the agent's scratchpad.

## Workflow rules from the user (keep following)

- Backports only via `git cherry-pick -m1`; Dash adaptations as own non-fixup
  commits when non-trivial, otherwise `fixup!` of the backport. No uber
  commits. Never squash while something is still failing.
- No big rebases unless a full recompile is coming anyway.
- Resources: at most 16 compile jobs and `-j15` for functional tests, and
  less when others are building (16 GB RAM; check `uptime`, `free -g` and
  `df -h /home` first). `make -j12` is the safe default when idle.
- Few parallel agents; do the work directly.
- Always full `make -jN`, never individual targets.

## Disk-full lessons (this VM)

`/home` (btrfs, 120 GB) fills up: `~/bc` is a mainnet datadir (~55 GB), there
are four checkouts with debug builds, ccache caps at 5 GB. One debug build
holds ~4.2 GB of linked binaries (test_dash ~1 GB, dash-qt, test_dash-qt,
fuzz, bench_dash, dashd) plus ~0.7 GB archives that `ar` rewrites through a
temporary copy. A full relink needs ~4 GB free plus headroom; 3.6 GB is not
enough.

When a build dies with "No space left on device" in `ar`, the archive it was
writing is truncated even if its size looks normal, and make then treats it
as up to date: the next build fails with `ld: error: lib*.a: file too short`
or undefined references. Delete the archives named in the failing run
(`libbitcoin_node.a`, `libbitcoin_zmq.a`, `libtest_util.a`,
`libbitcoin_wallet_tool.a` happened) and rerun make. Only this checkout's
own stale binaries are mine to delete; never touch `~/bc`, ccache or the
other checkouts without asking.

`make check` regenerates `clientversion.o` whenever HEAD changes and relinks
every binary; do not start functional tests while it runs or they find no
`dashd`.

## Useful commands

```bash
./src/test/test_dash --run_test=script_tests,script_standard_tests,descriptor_tests,key_io_tests,key_tests,transaction_tests,psbt_wallet_tests,wallet_tests,walletload_tests
test/functional/test_runner.py -j4 "feature_taproot.py --descriptors" "wallet_taproot.py --descriptors" "rpc_psbt.py --descriptors" rpc_decodescript.py
test/lint/all-lint.py
```

## DIP-0143 (dashpay/dips#149) versus the taproot sighash

They are different algorithms for different signature types. DIP-0143 is
the Dash analogue of BIP143: a digest for ECDSA signatures in legacy
scripts (P2PKH/P2SH), signalled by a `SIGHASH_DIP0143` flag bit, gated by
`SCRIPT_ENABLE_DIP0143` under an EHF hard fork. The taproot digest on this
branch is BIP341's (Schnorr, key path and tapscript) with the Dash version
word and `vExtraPayload` added, exactly the two Dash additions DIP-0143
makes. Both share the per-transaction midstate hashes that
`PrecomputedTransactionData` already computes (prevouts, sequences,
outputs; BIP341 additionally amounts and scriptPubKeys), so a DIP-0143
implementation would reuse that cache and add a `SigVersion` for the legacy
ECDSA path: the taproot work does not replace it and does not need it.

## Verification on develop 6271b740e9 (2026-11-10)

Regular debug build with zero warnings; lint clean (minus the uninstalled
tools); `make check` green including `test_dash-qt` (with dash#8080 on top)
and all 172 `test_dash` suites; the functional set of 29 tests (feature_taproot,
wallet_taproot, wallet_signer, rpc_psbt, rpc_decodescript, feature_block,
feature_csv_activation, mempool_accept, rpc_createmultisig, rpc_misc,
rpc_rawtransaction, rpc_signrawtransactionwithkey, interface_bitcoin_cli,
tool_wallet and the wallet_* descriptor suite) all passed.

## Own deployment (2026-11-10)

v24 is released, so taproot moved from `DEPLOYMENT_V24` to its own
`DEPLOYMENT_TAPROOT` (bit 15, `"taproot"` in `getblockchaininfo`, EHF like
v24/evo_shares, same window and thresholds). Mainnet start 2027-05-10 and
testnet/devnet start 2027-03-01 are placeholders for release time; regtest
starts at 0. Masternodes sign its MNHF signal like v24's (no `evo_shares`-style
skip in `ehf_signals.cpp`). Gating sites: `GetBlockScriptFlags` and the
mempool `AreInputsStandard` call in `validation.cpp`, and
`Chain::isTaprootActive()` for the wallet (default `tr()` descriptors,
`importdescriptors`, external signers). Tests use `-vbparams=taproot:...`.
