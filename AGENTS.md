# Dash Core Agent Guide

This file is for automated coding agents working in Dash Core. Keep it
practical: prefer local source, tests, and project history over guesses.

`AGENTS.md` and `CLAUDE.md` intentionally contain the same guidance. When one
changes, update the other in the same commit.

## First Principles

- Understand the code path before editing. Read callers, callees, tests, and
  recent history for the touched files.
- Keep changes narrow. Do not mix cleanup, formatting, refactors, and behavior
  changes unless the task explicitly asks for it.
- Preserve Dash-specific behavior when backporting or refactoring Bitcoin Core
  code. Dash consensus, masternodes, LLMQs, ChainLocks, InstantSend, Platform
  credit-pool logic, governance, and sporks often extend the upstream path.
- Do not add symlinks. `contrib/devtools/github-merge.py` rejects symlinks in
  the tree during merge.
- Do not edit generated release artifacts, Guix/release files, vendored code, or
  translations unless the task is specifically about those files.
- Code should be readable on its own. Needing a significant comment usually
  means the code itself should be clearer. Avoid comments that restate the
  code; reserve them for things that genuinely need explaining (non-obvious
  invariants, workaround rationale, non-local side effects).

## Assertions and Checks

Full guidance lives in `doc/developer-notes.md` under "Assertions and Checks".
Short version, in order of preference:

- `Assume(cond)` is the default. Use it for "this is how things are supposed to
  be": a violation means someone has a bug worth investigating, but execution
  stays well-defined. A negative rate-limit counter is the archetype - somebody
  decremented twice, we may be more DoS-exposed than intended, but nothing is
  corrupt. It aborts in `--enable-debug` and `--enable-fuzz` builds (CI's
  `linux64_multiprocess` and fuzz jobs) while a failure is silent in release,
  so it must never take down a production node. The expression is always
  evaluated.
- `assert(cond)` / `Assert(cond)` is the "we must crash now" case. Use it only
  when continuing would be undefined behavior, memory corruption, or corrupt
  persisted/consensus state - aborting has to be the safer outcome. It should
  be rare and obviously justified, but do use it where it is genuinely needed
  to document and enforce a precondition that keeps the code below it safe.
  `Assert` returns its argument: `assert(ptr != nullptr); obj = *ptr;` becomes
  `obj = *Assert(ptr);`
- `CHECK_NONFATAL(cond)` / `NONFATAL_UNREACHABLE()` for internal logic bugs on
  a path with a caller to report to. Required in RPC code, enforced
  (best-effort) for `src/rpc/` and `src/wallet/rpc*`.

The production-crash guidance above does not apply to C++ regression and
unit-test sources under `src/test/`, `src/qt/test/`, `src/wallet/test/`. They compile into test
binaries, not user-facing `dashd` or `dash-qt`; `assert`, `Assert`, `Assume`,
and related fatal test checks are all acceptable. Do not flag the choice among
them as a production-crash risk.

None of these validate input. Data from peers, RPC arguments, wallet files, or
on-disk state must be checked and rejected through normal error handling -
asserting on it turns a peer-triggered inconsistency into a remote crash.
Environment failures (disk full, corrupt block on disk, failed DB write) are
not checks at all: return an error, `AbortNode()`, or `InitError()`.

## Repository Map

- `src/` - C++ implementation.
- `src/bench/` - benchmarks.
- `src/index/`, `src/interfaces/`, `src/node/`, `src/rpc/`, `src/wallet/` -
  subsystem code inherited mostly from Bitcoin Core.
- `src/llmq/`, `src/masternode/`, `src/evo/`, `src/governance/`,
  `src/coinjoin/`, `src/instantsend/`, `src/spork*` - Dash-specific systems.
- `src/test/`, `src/wallet/test/`, `src/qt/test/` - C++ unit tests.
- `test/functional/` - Python functional tests for `dashd`.
- `test/lint/` - static checks.
- `depends/` - dependency build system.
- `ci/`, `.github/` - CI entry points and GitHub workflows.
- `doc/` - user and developer documentation.
- `contrib/` - scripts and release/maintenance tooling.

Vendored or subtree-style code should normally be left alone:

- `src/{crc32c,dashbls,gsl,immer,leveldb,minisketch,secp256k1,univalue}`
- `src/crypto/{ctaes,x11}`

`test/util/data/non-backported.txt` lists Dash-specific files used by Dash
style/lint checks such as clang-format-diff and cppcheck. Do not treat it as a
list of skipped upstream backport hunks.

## Build Commands

Use portable parallelism in examples. Linux-only CPU-count helpers are not
available on every supported developer host.

```bash
./autogen.sh

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
JOBS="$(( JOBS > 1 ? JOBS - 1 : 1 ))"

make -C depends -j"$JOBS"

# Use the depends prefix printed for your platform, for example
# depends/x86_64-pc-linux-gnu or depends/aarch64-apple-darwin24.3.0.
./configure --prefix="$(pwd)/depends/[platform-triplet]"

make -j"$JOBS"
```

Useful developer configure flags:

```bash
./configure --prefix="$(pwd)/depends/[platform-triplet]" \
            --disable-hardening \
            --enable-crash-hooks \
            --enable-debug \
            --enable-reduce-exports \
            --enable-stacktraces \
            --enable-suppress-external-warnings \
            --enable-werror
```

Generate `compile_commands.json` or run clang-tidy:

See `doc/developer-notes.md` under "Running clang-tidy".

When adding, removing, or renaming C++ source files, update the build system in
the same change. Most source/test files need `src/Makefile.am` or
`src/Makefile.test.include` updates.

## Writing Tests

Pick the test type by what it can observe, not by where it is easiest to
write.

- A unit test (`src/test/`, `src/qt/test`, `src/wallet/test`, Boost) isolates
  one function or class. Every input is named in the test and every assertion
  checks its documented return value or state. If the test needs a full node fixture,
  injected internal state, a `friend` declaration, or an explanation of
  how a private method computes its precondition, it is not a unit test.
  Extract the logic into a directly testable function or write a functional test.
- A functional test (`test/functional/`, Python) proves a user-visible outcome
  through RPC or P2P: a block is accepted, a lock appears, a peer is or is not
  banned. It is the right home for anything that depends on quorums, signing,
  sync state, or several subsystems together.
- A regression test, of either kind, must fail without the fix and pass with
  it, and it must observe the behavior the change claims. Asserting on a
  cache entry, a seen-set size, or a returned container because the real
  outcome is unreachable in the fixture is not coverage. If the real outcome
  cannot be observed at that layer, test at the layer where it can.
- One scenario per test case, or a table of inputs with the expected value
  beside each. Do not chain unrelated scenarios in one case with a single
  trailing assertion; a failure must point at the scenario that broke.
  Assertions inside a shared lambda hide which call failed.
- Prefer existing suites and files; each new file adds setup and compile
  time. A new file is justified only by a self-contained feature with its
  own setup, or when a separate file yields clearly better isolation or
  parallelism, not by a new variant of a scenario a neighbouring test
  already covers. Name tests after the invariant they check, not after the
  PR or a vague adjective.
- Every scenario the code special-cases needs its own negative case. Ten
  gates checked by one "nothing changed" comparison at the end is one test,
  not ten.
- A PR may claim only the verification that was actually run. Mutation checks
  and "fails without the fix" claims belong in the PR only when they were
  performed and can be reproduced from the description.
- Some changes cannot be tested honestly at either level: performance work,
  races and other multi-threading bugs, timing- or scheduler-dependent
  behavior, anything whose failure is non-deterministic. A test that needs
  sleeps, retries, or a harness more intricate than the fix itself is not
  worth its upkeep. Say in the PR how the change was verified instead of
  faking a test.
- Some changes need no test because the diff is the proof: passing by
  reference instead of by value, a typo or a stray or missing newline in a
  log line, a wrong log category, a misspelled RPC help string, a missing
  `const`, a renamed local, an include reordering. Do not write a test whose
  only purpose is to satisfy the checklist.
- Every line of test code is a maintenance obligation for as long as the
  test exists. Keep tests compact, drive them through public interfaces and
  observable outcomes rather than internal structures or private members,
  and add one only when catching a regression is worth carrying that test
  for years. When it is not, leave it out and say so.

## Test Commands

Choose tests based on the files touched. Do not claim broad validation if only a
targeted test was run.

```bash
# All unit tests
make check

# One Boost test suite or case
./src/test/test_dash --run_test=getarg_tests

# All functional tests
test/functional/test_runner.py

# One functional test
test/functional/test_runner.py wallet_hd.py

# Parallel functional tests
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
test/functional/test_runner.py -j"$JOBS"

# Lint
test/lint/all-lint.py
test/lint/lint-python.py
test/lint/lint-shell.py
test/lint/lint-whitespace.py
test/lint/lint-circular-dependencies.py
```

Functional-test prerequisites and usage details live in `test/README.md`.
Several Dash-specific tests need the `dash_hash` Python package.

## Backport Work

Dash Core regularly backports Bitcoin Core changes. Treat backports as
source-history work, not only conflict resolution.

- Identify the exact upstream Bitcoin Core PR(s) and commit(s).
- Keep upstream backport commits as close to 1:1 as practical. Put shared Dash
  repair work on a staging/base branch instead of hiding it inside an unrelated
  upstream backport commit.
- When reviewing Bitcoin Core backports, absent a clear bug, prefer staying
  aligned with upstream. Do not request Dash-only policy or style changes, such
  as replacing an assertion primitive solely to match this guide. Dash-specific
  correctness, security, or consensus issues are valid reasons to adapt
  upstream code.
- Compare the upstream diff to the Dash diff file by file.
- Check prerequisite PRs. If an upstream hunk depends on a helper, test, type,
  or file introduced by an earlier Bitcoin PR, either backport the prerequisite
  or document why the hunk is intentionally excluded.
- Do not silently drop upstream tests. If a test depends on a missing
  prerequisite, call that out in the PR description or add the prerequisite.
- If a backport is partial, explain the omitted upstream commits, hunks, or
  tests in the commit or PR text.
- Verify the PR title/body matches the actual commits still reachable from the
  branch. Stale "backports X" metadata has caused bad reviews.
- Keep Dash adaptations explicit. When upstream code touches a path that Dash
  has extended, inspect the Dash-specific logic before accepting the upstream
  shape.
- Resolve conflicts against Dash APIs, not only upstream structure. A backport
  that textually resembles Bitcoin Core can still fail to compile or lose Dash
  behavior if Dash-only overloads, helpers, or wallet paths are removed.

## Dash-Specific Review Hotspots

Be extra careful around:

- consensus and script flags;
- special transaction payload serialization;
- deterministic masternode list updates;
- LLMQ DKG, signing sessions, recovered signatures, and quorum rotation;
- InstantSend and ChainLocks request/relay paths;
- governance object and superblock payment logic;
- EvoDB, credit-pool, asset-lock, and Platform integration code;
- network-message serialization, checksums, and partial-send paths;
- BLS scheme transitions across connect, disconnect, undo, and activation
  boundaries;
- future DKG/quorum prediction, which must evaluate quorum availability at the
  relevant future work/cycle base instead of only the current tip;
- time, mocktime, scheduler, and interrupt/shutdown behavior.

For these areas, prefer small tests that prove the invariant being changed.

## PR Hygiene

- When creating pull requests, follow `.github/PULL_REQUEST_TEMPLATE.md` for the description and ensure the PR title satisfies the active linter in `.github/workflows/semantic-pull-request.yml` (using Conventional Commits, including `backport:` for Bitcoin Core backports).
- Use atomic commits. Each commit should make sense on its own and generally
  build and pass tests. An intentionally non-building commit (e.g. a
  regression test landing before its fix) is fine if called out explicitly so
  it isn't mistaken for an oversight.
- Remove the italicized helper prompts from `.github/PULL_REQUEST_TEMPLATE.md`, fill in the required sections, and keep the checklist accurate for the change.
- Do not put `@` mentions in PR descriptions; they are copied into merge
  commits and notify users repeatedly.
- Explain what changed and why. For bug fixes, include the failure mode and why
  the chosen fix is correct.
- If CI fails for reasons unrelated to the PR, document the evidence instead of
  pushing empty commits or unrelated changes.
- For depends/cache failures, inspect both the cache-producing and
  cache-consuming jobs. Rerunning only the failed consumer can preserve the same
  missing-cache failure.

## Local Debugging

```bash
# Run dashd with broad logging
./src/dashd -debug=all -printtoconsole

# Run a functional test against a custom binary
test/functional/test_runner.py --dashd=/path/to/dashd wallet_hd.py

# Keep failed functional-test datadirs
test/functional/test_runner.py --nocleanup --tracerpc -l DEBUG wallet_hd.py

# Debug a unit-test binary
gdb ./src/test/test_dash

# Profile a functional test
test/functional/test_runner.py --perf wallet_hd.py
perf report -i /path/to/datadir/test.perf.data --stdio | c++filt
```

## When In Doubt

- Read `CONTRIBUTING.md`, `doc/developer-notes.md`, and nearby tests.
- Search for similar code with `rg` before inventing a new pattern.
- Prefer established project helpers over ad hoc parsing or shell tricks.
- Leave a clear note in the PR when a choice is deliberate and could otherwise
  look like an omission.
