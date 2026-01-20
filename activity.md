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
- [!] Implement `test_verifyquorumproofchain_success()` - BLOCKED: Bug found in C++ BuildProofChain

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

## Resolved: Chainlock Indexing Bug

**Root Cause Found:** The `ActiveChain().Contains(pindex)` check in `specialtxman.cpp:670` always returned false during block connection because the chain tip is updated AFTER `ProcessSpecialTxsInBlock` returns.

**Fix:** Removed the incorrect `ActiveChain().Contains(pindex)` check since `!fJustCheck` is sufficient to distinguish real block connection from validation-only.

## Blocked: BuildProofChain Signer Detection Bug

**Issue Found:** The `BuildProofChain` function in `quorumproofs.cpp` incorrectly identifies which quorum signed a chainlock.

**Details:**
- `BuildProofChain` searches for a chainlock at heights from `pMinedBlock->nHeight` to `maxSearchHeight`
- For each height, it computes which commitment WOULD sign using `cachedCommitments`
- The `cachedCommitments` are fetched from the chain at `pMinedBlock->nHeight - SIGN_HEIGHT_OFFSET`
- But the actual chainlock at a higher height might have been signed by DIFFERENT active commitments
- The function claims `KnownSigner=1` but verification fails because the actual signer differs

**Root Cause:** The code assumes commitments active at the mined block height are the same as those that actually signed the chainlock at a later height. With quorum rotation, this assumption is incorrect.

**Location:** `src/llmq/quorumproofs.cpp:655-696` (the search loop in `BuildProofChain`)

**Impact:** Proof chain verification fails when the target quorum is mined multiple DKG cycles after the checkpoint quorums. The proof generator thinks it found a valid signer but the actual chainlock was signed by a rotated-out quorum.
