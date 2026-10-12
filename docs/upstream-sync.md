# Staying in sync with upstream

This repository is a fork of [`paolostivanin/OTPClient`](https://github.com/paolostivanin/OTPClient).

| Remote | URL | Role |
|---|---|---|
| `origin` | `git@github.com:sandhilt/OTPClient-windows.git` | the fork |
| `upstream` | `git@github.com:paolostivanin/OTPClient.git` | source of truth for all non-Windows behaviour |

State when this was written (2026-10-12): `master` equals `upstream/master` (v5.2.2), 0 commits ahead, 0 behind. Fork work starts from a clean slate.

## Where upstream churns

Files touched most in the last 300 upstream commits (`git log --format= --name-only -n 300 upstream/master | sort | uniq -c | sort -rn`):

| Commits | File |
|---:|---|
| 81 | `src/gui/otpclient-window.c` |
| 60 | `src/gui/otpclient-application.c` |
| 55 | `CMakeLists.txt` |
| 46 | `data/…appdata.xml` |
| 33 | `src/search-provider/search-provider.c` |
| 33 | `src/common/db-common.c` |
| 32 | `src/cli/exec-action.c` |
| 31 | `README.md` |
| 28 | `src/gui/dialogs/settings-dialog.c` |
| 23 / 22 / 22 | `src/common/common.c`, `aegis.c`, `src/cli/main.c` |

Every one of these is a merge-conflict magnet. They are also exactly where Windows-specific edits would naturally go (window, application, root CMake, common.c). That tension drives the rules below.

Re-measure before a big refactor; the list moves over time.

## Rules

1. **Do not move, rename, reformat or re-indent upstream files.** A rename turns every future upstream change to that file into a conflict.
2. **Fork-only code lives in new paths** that upstream will never touch: `src/platform/` (see `docs/windows-port.md`), `cmake/`, `packaging/windows/`, `docs/`, `AGENTS.md`, and the Nix dev environment (`flake.nix`, `flake.lock`, `.envrc`). The only upstream file touched so far besides the README notice is `.gitignore` (a short block appended at the end: `.direnv/`, `/result`).
3. **Edits to upstream files stay small and mechanical:** a one-line include swap, one function call replacing an inline block, one `add_subdirectory`. No logic rewrites inside the churn hotspots above.
4. **Prefer shims over `#ifdef`.** A Windows-only shim header or library that satisfies an existing include (`<sys/file.h>`, `<uuid/uuid.h>`, `<libsecret/secret.h>`…) requires zero edits to the call sites. When an `#ifdef _WIN32` is unavoidable, keep it to a single line that dispatches into `src/platform/`, never a block of Windows logic inside an upstream file.
5. **Keep upstream behaviour the default.** The Linux build must stay byte-for-byte the same configuration. Windows opt-in goes through `if(WIN32)` in CMake and the platform layer.
6. **`README.md` and the appdata file are churn hotspots too.** The fork's notes live in one clearly delimited block near the top of the README so upstream edits merge around it.

## Sync procedure

```sh
git fetch upstream --tags
git log --oneline master..upstream/master        # what is coming
git diff --stat master...upstream/master -- src/gui/otpclient-window.c src/gui/otpclient-application.c CMakeLists.txt
git merge upstream/master                        # merge, not rebase: keeps the fork history stable
```

- Use **merge**, not rebase, on `master`: it keeps conflict resolutions recorded and avoids rewriting published history.
- After merging, rebuild Linux (`AGENTS.md` lists the commands) **and** Windows, run the test suite, then re-check the "seam call sites" list in `docs/windows-port.md`. Upstream refactors may add a new `prctl`, `flock`, libsecret or GSettings call site that needs routing through the platform layer.
- Run `git grep -nE "prctl|flock|/proc/self|libsecret|termios|sigaction"` on the merged tree and compare against `docs/platform-dependencies.md` to catch new Linux-only calls early.
- Upstream version bumps touch `CMakeLists.txt` (`project(... VERSION ...)`), the appdata release list and `README.md`. Take upstream's side and re-apply the fork block if needed.

## Contributing back

Anything that is not Windows-specific (a portability fix, a missing `static`, a test gap) is a candidate for an upstream PR. Land it upstream first when practical, then merge it back; the fork diff shrinks each time.
