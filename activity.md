# Activity Log - Quorum Proof Chain Tests

## Status Legend
- [ ] Not started
- [x] Completed
- [!] Failed/Blocked

## Tasks

### Phase 1: Fix Existing Infrastructure
- [x] Fix `build_checkpoint()` to use LLMQ type 100 instead of 104

### Phase 2: Add Helper Methods
- [x] Add `tamper_proof_hex()` helper method

### Phase 3: Single-Step Proof Chain Tests
- [x] Implement `test_getquorumproofchain_single_step()` (required bug fix in specialtxman.cpp)
- [x] Implement `test_verifyquorumproofchain_success()` - Fixed bugs in BuildProofChain and VerifyProofChain

### Phase 4: Verification Failure Tests
- [ ] Implement `test_verifyquorumproofchain_tampered()`
- [ ] Implement `test_verifyquorumproofchain_wrong_target()`
- [ ] Implement `test_verifyquorumproofchain_wrong_checkpoint()`

### Phase 5: Error Handling Tests
- [ ] Implement `test_getquorumproofchain_errors()`

### Phase 6: Multi-Step Proof Chain Tests
- [ ] Implement `test_getquorumproofchain_multi_step()`

### Phase 7: Integration
- [ ] Update `run_test()` to call all new test methods in correct order
- [ ] Final verification - run full test suite

## Completion Log

| Date | Task | Status | Commit |
|------|------|--------|--------|
| 2026-01-20 | Fix build_checkpoint() | Completed | a2d5aad7505 |
| 2026-01-20 | Add tamper_proof_hex() helper | Completed | d1cf5759ff9 |
| 2026-01-20 | Fix chainlock indexing bug | Completed | 4b0d4c10b28 |
| 2026-01-20 | test_getquorumproofchain_single_step | Completed | ec75ae26ec5 |
| 2026-01-20 | Fix BuildProofChain & VerifyProofChain bugs | Completed | pending |
| 2026-01-20 | test_verifyquorumproofchain_success | Completed | pending |

## Resolved: Chainlock Indexing Bug

**Root Cause Found:** The `ActiveChain().Contains(pindex)` check in `specialtxman.cpp:670` always returned false during block connection because the chain tip is updated AFTER `ProcessSpecialTxsInBlock` returns.

**Fix:** Removed the incorrect `ActiveChain().Contains(pindex)` check since `!fJustCheck` is sufficient to distinguish real block connection from validation-only.

## Resolved: BuildProofChain and VerifyProofChain Bugs

### Bug 1: BuildProofChain Signer Detection

**Issue:** The `BuildProofChain` function incorrectly identified which quorum signed a chainlock due to caching commitments at a fixed reference height.

**Root Cause:** The code cached commitments at `pMinedBlock->nHeight - SIGN_HEIGHT_OFFSET` and used them for ALL heights in the search window. With quorum rotation, the active commitments at a later chainlock height can differ.

**Fix:** Use `DetermineChainlockSigningCommitment(h, ...)` for each height `h` in the search loop, which correctly computes the signing commitment by looking at commitments at `h - SIGN_HEIGHT_OFFSET`.

### Bug 2: VerifyProofChain Signature Verification

**Issue:** Chainlock signature verification used `blockHash` directly as the message, but chainlocks are signed using `SignHash(llmqType, quorumHash, requestId, blockHash)`.

**Root Cause:** The verifier tried to verify signatures against all known public keys using only the block hash, ignoring the SignHash construction that includes the quorum hash and request ID.

**Fix:**
1. Added `signingQuorumHash` and `signingQuorumType` fields to `ChainlockProofEntry`
2. Updated verification to build proper `SignHash` using `chainlock::GenSigRequestId(height)`
3. Verify against the specific signing quorum's public key instead of all known keys
