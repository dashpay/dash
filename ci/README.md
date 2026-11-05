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

### amd64 runner tiers

`.github/workflows/select_dynamic_runner.py` places the amd64 jobs in two
tiers, each with its own self-hosted runner label. The jobs keep their names
and steps, so required checks are unchanged.

| Tier | Jobs | Label variable |
| --- | --- | --- |
| fast | lint, cppcheck, cache-sources, the amd64 pull request container builds and manifests, and the `linux64` depends, build and test | `RUNNER_SELFHOSTED_FAST` |
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
- With `RUNNER_AMD64` unset, the Blacksmith step is GitHub-hosted.
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
