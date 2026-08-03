This folder contains lint scripts.

Running locally
===============

To run linters locally with the same versions as the CI environment, use the included
Dockerfile:

```sh
DOCKER_BUILDKIT=1 docker build -t bitcoin-linter --file "./ci/lint_imagefile" ./

docker run --rm -v $(pwd):/bitcoin -it bitcoin-linter
```

After building the container once, you can simply run the last command any time you
want to lint.

Reproducing the CI lint job
---------------------------

The image above is not what CI uses: its default entrypoint runs `ci/lint/06_script.sh`,
which merge-bases against `master` and runs checks (`check-doc.py`, subtree checks) that
the CI lint job does not, and it has no cppcheck. The CI lint job
(`.github/workflows/lint.yml`) runs `ci/dash/lint.sh` inside the `ci-slim` image. To
reproduce it, build that image and run the same script with the same environment from the
repository or worktree root:

```sh
docker build -t dash-ci-slim --file ./contrib/containers/ci/ci-slim.Dockerfile ./contrib/containers/ci

# Commit or stash tracked changes first: commit-script-check.sh checks out
# commits, runs `git reset --hard`, and executes the verification commands of
# `scripted-diff:` commits in the range, so only run it on commits you trust.
GIT_COMMON_DIR="$(git rev-parse --path-format=absolute --git-common-dir)"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$PWD":"$PWD" -v "$GIT_COMMON_DIR":"$GIT_COMMON_DIR" -w "$PWD" \
    -e CACHE_DIR="$PWD/ci/scratch/cache" -e BUILD_TARGET=linux64 -e CHECK_DOC=1 \
    -e PULL_REQUEST=true -e COMMIT_RANGE="$(git merge-base develop HEAD)..HEAD" \
    dash-ci-slim bash -c 'git config --global --add safe.directory "$PWD" && ./ci/dash/lint.sh'
```

CI builds `ci-slim` for both `linux/amd64` and `linux/arm64`, so a native build is fine;
rebuild it whenever `ci-slim.Dockerfile` changes. Unlike CI, the container runs as your
user instead of root so files it writes stay owned by you, and the second mount is what
makes it work from a git worktree. `COMMIT_RANGE` uses your local `develop`, so keep that
branch current with `dashpay/dash`. The cppcheck cache is kept in the git-ignored
`ci/scratch/cache/` directory to speed up reruns.


check-doc.py
============
Check for missing documentation of command line options.

commit-script-check.sh
======================
Verification of [scripted diffs](/doc/developer-notes.md#scripted-diffs).
Scripted diffs are only assumed to run on the latest LTS release of Ubuntu. Running them on other operating systems
might require installing GNU tools, such as GNU sed.

git-subtree-check.sh
====================
Run this script from the root of the repository to verify that a subtree matches the contents of
the commit it claims to have been updated to.

```
Usage: test/lint/git-subtree-check.sh [-r] DIR [COMMIT]
       test/lint/git-subtree-check.sh -?
```

- `DIR` is the prefix within the repository to check.
- `COMMIT` is the commit to check, if it is not provided, HEAD will be used.
- `-r` checks that subtree commit is present in repository.

To do a full check with `-r`, make sure that you have fetched the upstream repository branch in which the subtree is
maintained:
* for `src/secp256k1`: https://github.com/bitcoin-core/secp256k1.git (branch master)
* for `src/leveldb`: https://github.com/bitcoin-core/leveldb-subtree.git (branch bitcoin-fork)
* for `src/crypto/ctaes`: https://github.com/bitcoin-core/ctaes.git (branch master)
* for `src/crc32c`: https://github.com/bitcoin-core/crc32c-subtree.git (branch bitcoin-fork)
* for `src/minisketch`: https://github.com/sipa/minisketch.git (branch master)

To do so, add the upstream repository as remote:

```
git remote add --fetch secp256k1 https://github.com/bitcoin-core/secp256k1.git
```

all-lint.py
===========
Calls other scripts with the `lint-` prefix.


lint_ignore_dirs.py
===================
Add list of common directories to ignore when running tests
