## CI Scripts

This directory contains scripts for each build step in each build stage.

### Configurations

Configurations live in `ci/test/00_setup_env*.sh` and are selected by
`BUILD_TARGET` in `ci/dash/matrix.sh`, driven by the GitHub Actions workflows
in `.github/workflows`. They are constructed to test a wide range of
configurations, rather than a single pass/fail. This helps to catch build
failures and logic errors that present on platforms other than the ones the
author has tested.

Some builders use the dependency-generator in `./depends`, rather than using
the system package manager to install build dependencies. This guarantees that
the tester is using the same versions as the release builds, which also use
`./depends`.

If values are left out, `00_setup_env.sh` is used as the default configuration
with fallback values.

### Cache

In order to avoid rebuilding all dependencies for each build, the binaries are
cached and reused when possible. Changes in the dependency-generator will
trigger cache-invalidation and rebuilds as necessary.

### clang-tidy result cache

`linux64_multiprocess-lint` restores only `/cache/ctcache`, separately from
compiler ccache, cppcheck and depends. Its `CTCACHE_MAXSIZE_MB` is 512 MiB:
a full lint pass currently occupies about 237 MiB, so this retains a full
pass with room for changed inputs. Other jobs' cache limits and the local
fallback in `00_setup_env.sh` are unchanged. `cleanup-ctcache.py` removes
oldest files until allocated disk usage is within the limit, rather than
removing a percentage of files with very different sizes. ctcache refreshes
an output's modification time on a hit.

The archive key includes both CI Dockerfiles (including the inherited LLVM
version) and the bitcoin-tidy plugin sources. Within that namespace, restore
falls back to the latest commit's archive. Do not add all depends recipes to
the namespace: ctcache already hashes preprocessed source and headers,
comments (for NOLINT), compiler/tidy arguments and the effective tidy config.
A recipe change should invalidate affected translation units, not every
cached result. Toolchain/plugin changes still require a cold cache.

Only successful default-branch push jobs save archives; pull requests only
restore them. The existing `prune-ccache` job keeps the newest default-branch
ctcache generation per target. Overlapping runs can restore before a previous
run saves, so cold starts are still possible. Separate keys do not reserve
separate GitHub cache quotas: all jobs share the repository's storage limit
(10 GB by default), with least-recently-accessed eviction and removal of
entries unused for seven days. Account for overlapping generations and other
jobs before increasing this budget further.

Run the cleanup regression tests without a compiler or build artifacts:

```sh
python3 -m unittest discover -s ci/dash -p test_cleanup_ctcache.py -v
```

### amd64 runner tiers

`.github/workflows/select_dynamic_runner.py` places the amd64 jobs in two
tiers, each with its own self-hosted runner label. The jobs keep their names
and steps, so required checks are unchanged.

| Tier | Jobs | Label variable |
| --- | --- | --- |
| fast | lint, cppcheck, cache-sources, the amd64 pull request container builds and manifests, the `linux64` and `linux64_platform_gui` depends, and the `linux64` build and test | `RUNNER_SELFHOSTED_FAST` |
| build | every other amd64 depends, build and test job (`aarch64-linux`, `linux64_asan`, `linux64_fuzz`, `linux64_nowallet`, `mac`, `win64`) | `RUNNER_SELFHOSTED_BUILD` |

Each tier picks the first of these that applies:

| | fast tier | build tier |
| --- | --- | --- |
| GitHub-hosted | no hosted job queued | fewer than `BUILD_HOSTED_QUEUE_LIMIT` (20) hosted jobs queued |
| its self-hosted label | fewer than `FAST_SELFHOSTED_QUEUE_LIMIT` (16) jobs queued for it | fewer than `BUILD_SELFHOSTED_QUEUE_LIMIT` (24) jobs queued for it |
| `RUNNER_AMD64` (Blacksmith) | otherwise | otherwise |

The limits are set in `build.yml`. The hosted count covers every job queued
for GitHub-hosted runners across the repositories the selector scans.
The queue counts are a snapshot taken once per run, by the `Check skip
conditions` job: jobs a run will queue later are not counted, so one run can
still add a dozen or more jobs to a label at once. Two tiers that name the same
label share its queue.

- Only trusted runs may use a self-hosted label: a push to `develop` or
  `master`, a push whose actor is listed in `SELFHOSTED_LINT_AUTHORS`, or a pull
  request whose author and actor are listed and whose head repository is this
  one or is owned by a listed login. Every other run stays GitHub-hosted in
  both tiers unless it has the `blacksmith-ci` label.
- A `blacksmith-ci` label sends both tiers to `RUNNER_AMD64`. Otherwise a
  failed backlog measurement keeps both GitHub-hosted.
- With `RUNNER_AMD64` unset, the Blacksmith step uses `RUNNER_DEFAULT_AMD64`
  (or `ubuntu-24.04` when that variable is unset).
- Unsetting a tier's label variable skips only that tier's self-hosted step,
  for example to keep the build tier off a busy pool while the fast tier keeps
  using it. Jobs already queued for the label must be cancelled and re-run.
- The depends jobs build on the tier's runner, but their cache key still names
  the hosted ladder's runner, so every consumer restores the same entry.
- Container builds on push, and every arm64 job, keep the hosted ladder:
  GitHub-hosted, then `RUNNER_AMD64`/`RUNNER_ARM64` once more than
  `BACKLOG_THRESHOLD` (10) or `BACKLOG_THRESHOLD_ARM64` (30) hosted jobs are
  queued. The arm64 pull request container builds may use
  `RUNNER_SELFHOSTED_ARM64` for allowlisted runs.

Runners registered for either label must provide x86_64 Linux with a Docker
daemon (the jobs run in the CI container with `--cap-add SYS_PTRACE`), about 8
CPUs and 16 GiB of memory or more, roughly 40 GiB of free disk, and access to
GitHub's cache service and the remote ccache (`CCACHE_REMOTE_URL`). The fast
tier's label also runs Docker Buildx for the container builds, which push to
GHCR, and `make -C depends download` directly on the host for cache-sources. These runs
execute pull request code with access to that Docker daemon, so give each job
a fresh, single-use machine.

### Runner variables

Every workflow job picks its runner label from a repository or organization
variable, so a fork can move CI to other runners without editing workflows.
Unset variables keep the defaults below. Most of these workflows run pull
request code, and only the self-hosted labels below apply an author allowlist,
so a self-hosted label named by any other variable must be an isolated,
single-use machine.

| Variable | Workflow | Default |
|---|---|---|
| `RUNNER_DEFAULT_AMD64` | `build.yml` amd64 jobs | `ubuntu-24.04` |
| `RUNNER_DEFAULT_ARM64` | `build.yml` arm64 jobs and ccache pruning | `ubuntu-24.04-arm` |
| `RUNNER_AMD64`, `RUNNER_ARM64` | `build.yml` runs moved off the default runners by the backlog, the `blacksmith-ci` label or an amd64 tier's last step | the defaults above |
| `RUNNER_CHECK_SKIP` | `build.yml` `Check skip conditions`, the depends `Check cache` jobs and the existing-container checks | `ubuntu-24.04-arm` |
| `RUNNER_CACHE_DEPENDS_SOURCES` | scheduled `cache-depends-sources.yml` | `ubuntu-24.04-arm` |
| `RUNNER_GUIX_BUILD` | `guix-build.yml` image and build jobs | `ubuntu-24.04-arm` |
| `RUNNER_GUIX_ATTEST` | `guix-build.yml` release-tag `attest` job | `ubuntu-24.04` |
| `RUNNER_CLANG_DIFF_FORMAT` | `clang-diff-format.yml` | `ubuntu-latest` |
| `RUNNER_PREVENT_MASTER_PR` | `prevent-master-pr.yml` | `ubuntu-latest` |
| `RUNNER_LABEL_MERGE_CONFLICTS` | `label-merge-conflicts.yml` | `DASH_LIGHT_RUNNER`, then `ubuntu-latest` |
| `RUNNER_MERGE_CHECK` | `merge-check.yml` | `DASH_LIGHT_RUNNER`, then `ubuntu-latest` |
| `RUNNER_CHECK_MILESTONE` | `check-milestone.yml` | `DASH_LIGHT_RUNNER`, then `ubuntu-latest` |
| `RUNNER_PREDICT_CONFLICTS` | `predict-conflicts.yml` | `DASH_LIGHT_RUNNER`, then `ubuntu-latest` |
| `RUNNER_SEMANTIC_PULL_REQUEST` | `semantic-pull-request.yml` | `DASH_LIGHT_RUNNER`, then `ubuntu-latest` |
| `RUNNER_DOCKER_RELEASE` | `release_docker_hub.yml` | `ubuntu-22.04` |

`RUNNER_DEFAULT_AMD64` and `RUNNER_AMD64` must name x86_64 runners, and
`RUNNER_DEFAULT_ARM64` and `RUNNER_ARM64` aarch64 runners, because the
container images and depends cache keys are per architecture.
`RUNNER_DEFAULT_AMD64` also stands in for GitHub-hosted in the amd64 tiers
above: it is each tier's first step, where untrusted runs stay, and its
Blacksmith step while `RUNNER_AMD64` is unset.
`RUNNER_GUIX_BUILD` must resolve to runners of a single architecture, because
its build jobs run the image its image job builds.
Jobs without a `container:` run straight on the runner and expect the tools
GitHub's Ubuntu images ship, such as `gh`, `jq`, `python3` and Docker; ccache
pruning on `RUNNER_DEFAULT_ARM64`, for example, calls `gh` and `jq`.

The depends `Check cache` jobs run on `RUNNER_CHECK_SKIP`, and a cache hit
there skips the depends build. Blacksmith runners use their own cache rather
than GitHub's, so `RUNNER_CHECK_SKIP` and the runners that build depends must
use the same one: with Blacksmith default runners, set `RUNNER_CHECK_SKIP` to
a Blacksmith label and leave `RUNNER_SELFHOSTED_FAST` and
`RUNNER_SELFHOSTED_BUILD` unset.

The backlog that moves runs to `RUNNER_AMD64`/`RUNNER_ARM64` counts queued
jobs in the dashpay repositories listed in `select_dynamic_runner.py`, and
treats every label that is not `self-hosted`, a self-hosted label below, a
`blacksmith-` label or a default runner set to something other than one of
GitHub's image labels (such as `ubuntu-24.04` or `ubuntu-latest`) as
GitHub-hosted. A fork that sets the default runners
does not need to set `RUNNER_AMD64`/`RUNNER_ARM64`.

The self-hosted labels (`RUNNER_SELFHOSTED_FAST`, `RUNNER_SELFHOSTED_BUILD`,
`RUNNER_SELFHOSTED_ARM64`) are described above and in `build.yml`.
