# Staying in sync with upstream

This repository is a fork of [`paolostivanin/OTPClient`](https://github.com/paolostivanin/OTPClient).

| Remote | URL | Role |
|---|---|---|
| `origin` | `git@github.com:sandhilt/OTPClient-windows.git` | the fork |
| `upstream` | `git@github.com:paolostivanin/OTPClient.git` | source of truth for all non-Windows behaviour |

State when this was written (2026-10-12): `master` equals `upstream/master` (v5.2.2), 0 commits ahead, 0 behind. The `feat/windows-core-build` branch adds the Windows core + CLI layer; upstream application/build files remain unchanged.

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
2. **Fork-only code lives in new paths** that upstream will never touch: `src/platform/` (see `docs/windows-port.md`), `cmake/`, `packaging/windows/`, `docs/`, `AGENTS.md`, and the Nix dev environment (`flake.nix`, `flake.lock`, `.envrc`). Existing Windows paths are `src/platform/windows/` and `cmake/`; `src/platform/linux/` and `packaging/windows/` remain planned.
3. **Edits to upstream files stay small and mechanical:** a one-line include swap, one function call replacing an inline block, one `add_subdirectory`. No logic rewrites inside the churn hotspots above. Each such edit must be listed in the sections below.
4. **Prefer shims over `#ifdef`.** A Windows-only shim header or library that satisfies an existing include (`<sys/file.h>`, `<uuid/uuid.h>`, `<libsecret/secret.h>`…) requires zero edits to the call sites. When an `#ifdef _WIN32` is unavoidable, keep it to a single line that dispatches into `src/platform/`, never a block of Windows logic inside an upstream file.
5. **Keep upstream behaviour the default.** The Linux build must stay byte-for-byte the same configuration. Windows opt-in currently goes through `cmake/windows-ucrt.cmake`, its `CMAKE_PROJECT_INCLUDE` hook and the platform layer, without editing upstream CMake.
6. **`README.md` and the appdata file are churn hotspots too.** The fork's notes live in one clearly delimited block near the top of the README so upstream edits merge around it.

## Upstream files edited so far

Against local `master`, the only edited upstream files are:

- `README.md`: one delimited fork-notice block near the top.
- `.gitignore`: one appended direnv/Nix block (`.direnv/`, `/result`).

`AGENTS.md` is also in the filtered diff below, but is a new fork-only file. No upstream C source, header or CMake file is edited. Verify after each change:

```sh
git diff master --stat -- . ':!docs' ':!src/platform' ':!cmake' ':!flake.nix' ':!flake.lock' ':!.envrc'
```

### Planned routing edits (not yet made)

Not applied. Listed so each one is tracked before it lands.

- `src/cli/exec-action.c:149`: the `-p` refusal message ("Run `chmod 600 %s` and retry.") is the first candidate. It must route through `src/platform/` as a one-line call, and the Linux message must stay byte-identical.

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

Current candidates for **libcotp** upstream (implemented only as Windows nixpkgs package overrides in `flake.nix` today):

- Guard ELF-only linker options with `if(NOT APPLE AND NOT WIN32)` instead of `if(NOT APPLE)`.
- Add a `RUNTIME DESTINATION` for the Windows DLL. The current `postInstall` workaround finds `libcotp.dll` and installs it into `$out/bin`, failing if absent.

The other Windows-only overrides disable libgcrypt assembly (`--disable-asm`, MinGW rejects `--noexecstack`) and clear Jansson's ELF-oriented `postFixup` (Windows targets live in `out/cmake`, DLLs in `out/bin`; OTPClient uses pkg-config). Native Linux packages are not overridden.
