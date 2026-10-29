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

### Self-hosted fast path

When the repository variable `RUNNER_SELFHOSTED_FAST` names a runner label,
`.github/workflows/select_dynamic_runner.py` may move three jobs of the
`linux64` target from the hosted runner ladder (GitHub-hosted or Blacksmith)
to that label: the depends build (only on a depends cache miss),
`linux64-build` and `linux64-test`. They keep their names and steps, so
required checks are unchanged, and the depends cache key still names the
hosted runner, so the entry they save is the one the other linux64 builds
restore. Every other job stays where it was.

The decision is made once per run, by the `Check skip conditions` job. A run
takes the fast path only when all of these hold:

- it is a push to `develop` or `master`, or an event the pre-lint rung trusts:
  a push whose actor is listed in `SELFHOSTED_LINT_AUTHORS`, or a pull request
  whose author and actor are listed and whose head repository is this one or
  is owned by a listed login. Everything else, including fork pull requests
  from other contributors, stays on the hosted ladder;
- some GitHub-hosted job is queued (otherwise hosted starts at once);
- no `blacksmith-ci` label overrides the choice;
- at most `SELFHOSTED_FAST_MAX_QUEUED` (2, set in `build.yml`) jobs are queued
  for the label at that moment. Jobs already running there, or due to queue
  later in runs that already chose it, are not counted, so a burst of runs
  started together can still overfill a small pool.

Leaving the variable unset turns the fast path off. Jobs already queued for
the label must be cancelled and re-run after unsetting it.

Runners registered for the label must provide x86_64 Linux with a Docker
daemon (the jobs run in the CI container with `--cap-add SYS_PTRACE`), about 8
CPUs and 16 GiB of memory or more, roughly 40 GiB of free disk, and access to
GitHub's cache service and the remote ccache (`CCACHE_REMOTE_URL`). These runs
execute pull request code with access to that Docker daemon, so give each job
a fresh, single-use machine.

### Runner variables

Every workflow job picks its runner label from a repository or organization
variable, so a fork can move CI to other runners without editing workflows.
Unset variables keep the defaults below.

| Variable | Workflow | Default |
|---|---|---|
| `RUNNER_DEFAULT_AMD64` | `build.yml` amd64 jobs | `ubuntu-24.04` |
| `RUNNER_DEFAULT_ARM64` | `build.yml` arm64 jobs and ccache pruning | `ubuntu-24.04-arm` |
| `RUNNER_AMD64`, `RUNNER_ARM64` | `build.yml` runs moved off the default runners by the backlog or the `blacksmith-ci` label | the defaults above |
| `RUNNER_CHECK_SKIP` | `build.yml` `Check skip conditions`, the depends `Check cache` jobs and the existing-container checks | `ubuntu-24.04-arm` |
| `RUNNER_CACHE_DEPENDS_SOURCES` | scheduled `cache-depends-sources.yml` | `ubuntu-24.04-arm` |
| `RUNNER_GUIX_BUILD` | `guix-build.yml` | `ubuntu-24.04-arm` |
| `RUNNER_CLANG_DIFF_FORMAT` | `clang-diff-format.yml` | `ubuntu-latest` |
| `RUNNER_PREVENT_MASTER_PR` | `prevent-master-pr.yml` | `ubuntu-latest` |
| `RUNNER_LABEL_MERGE_CONFLICTS` | `label-merge-conflicts.yml` | `DASH_LIGHT_RUNNER`, then `ubuntu-latest` |
| `RUNNER_MERGE_CHECK` | `merge-check.yml` | `DASH_LIGHT_RUNNER`, then `ubuntu-latest` |
| `RUNNER_PREDICT_CONFLICTS` | `predict-conflicts.yml` | `DASH_LIGHT_RUNNER`, then `ubuntu-latest` |
| `RUNNER_SEMANTIC_PULL_REQUEST` | `semantic-pull-request.yml` | `DASH_LIGHT_RUNNER`, then `ubuntu-latest` |
| `RUNNER_DOCKER_RELEASE` | `release_docker_hub.yml` | `ubuntu-22.04` |

`RUNNER_DEFAULT_AMD64` and `RUNNER_AMD64` must name x86_64 runners, and
`RUNNER_DEFAULT_ARM64` and `RUNNER_ARM64` aarch64 runners, because the
container images and depends cache keys are per architecture.

The backlog that moves runs to `RUNNER_AMD64`/`RUNNER_ARM64` counts queued
jobs in the dashpay repositories listed in `select_dynamic_runner.py`, and
treats every label that is not `self-hosted`, a self-hosted rung or a
`blacksmith-` label as GitHub-hosted. A fork that sets the default runners
does not need to set `RUNNER_AMD64`/`RUNNER_ARM64`.

The self-hosted rungs (`RUNNER_SELFHOSTED`, `RUNNER_SELFHOSTED_ARM64`,
`RUNNER_SELFHOSTED_FAST`) are described above and in `build.yml`.
