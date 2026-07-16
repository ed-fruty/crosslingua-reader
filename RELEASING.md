# Releasing CrossLingua

Cutting a release is fully automated. You push a version tag; GitHub Actions
builds the firmware, generates a changelog, and publishes a real GitHub Release
with the binary attached. There is nothing to build or upload by hand.

This guide covers every future release.

---

## TL;DR

```sh
# From an up-to-date main, with a clean working tree:
git tag v1.2.0
git push origin v1.2.0
```

That's it. Watch the run under the repo's **Actions** tab; when it finishes the
release appears under **Releases** as **CrossLingua v1.2.0** with
`firmware.bin` attached and a changelog body.

---

## Prerequisites

- **You can push tags to `ed-fruty/crosslingua-reader`.** The workflow uses the
  built-in `GITHUB_TOKEN`; no personal secrets are needed.
- **Actions are enabled** for the repository (Settings → Actions).
- **The commit you tag is the exact code you want to ship.** Tag a commit that
  is already pushed to `main` (or whatever branch holds the release commit).
  The tag, not the branch, is what gets built.
- **Your history uses Conventional Commit messages** (`feat:`, `fix:`, …). The
  changelog is only as good as the commit messages — see
  [Writing changelog-friendly commits](#writing-changelog-friendly-commits).

---

## Choosing the version

Use [Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

- **MAJOR** (`v2.0.0`) — incompatible / breaking changes.
- **MINOR** (`v1.3.0`) — new features, backwards compatible.
- **PATCH** (`v1.2.1`) — bug fixes only.

**Tag format is `vX.Y.Z`** — a leading lowercase `v` followed by the semver
number. Pre-release tags such as `v1.3.0-rc.1` also match the trigger and will
publish a release; mark those as pre-releases if you use them (see
[Pre-releases](#pre-releases)).

Only tags matching `v*` trigger the pipeline. A tag like `1.2.0` (no `v`) or
`release-1.2.0` will **not** start a release.

---

## Cutting a release, step by step

1. **Make sure `main` is current and green**, and that the release commit is
   pushed:

   ```sh
   git checkout main
   git pull
   ```

2. **Create the tag.** Use the version you chose above. An annotated tag is
   recommended (it records who/when):

   ```sh
   git tag -a v1.2.0 -m "CrossLingua v1.2.0"
   ```

   A lightweight tag (`git tag v1.2.0`) works too.

3. **Push the tag:**

   ```sh
   git push origin v1.2.0
   ```

4. **Watch the build** under the **Actions** tab (workflow: **Release**). It
   takes a few minutes.

5. **Verify the result** under **Releases**: a release titled
   **CrossLingua v1.2.0**, a grouped changelog body, and `firmware.bin`
   attached.

Done.

---

## What the workflow does automatically

Defined in [`.github/workflows/release.yml`](.github/workflows/release.yml).
On a `v*` tag push it:

1. **Checks out** the repo with submodules (`open-x4-sdk`) and full history +
   tags (`fetch-depth: 0`, required for the changelog).
2. **Sets up** Python 3.14, `uv`, and the pinned pioarduino PlatformIO Core
   (`v6.1.19`).
3. **Rewrites the firmware version from the tag** (see below).
4. **Builds** the release firmware with `pio run -e gh_release`.
5. **Generates the changelog** with [git-cliff](https://git-cliff.org) using
   [`cliff.toml`](cliff.toml) — grouped by type (Features, Bug Fixes, …),
   covering commits **since the previous tag**.
6. **Publishes the GitHub Release** named `CrossLingua vX.Y.Z`, with the
   changelog as the body and `firmware.bin` attached.

You do not run any of these locally.

---

## How the version flows from the tag

The firmware embeds a compile-time version string, `CROSSPOINT_VERSION`, taken
from the `version` line in the `[crosspoint]` section of `platformio.ini`.

For a release, the workflow **rewrites that line to the tag with the leading
`v` stripped** before building:

```
tag v1.2.0  ->  version = 1.2.0  ->  -DCROSSPOINT_VERSION="1.2.0"
```

Key points:

- The rewrite happens **only in CI, on the throwaway checkout**. It is **never
  committed**. Your local `platformio.ini` keeps its own value
  (`1.0.0-fruty`), so dev builds are unaffected — only tagged release builds
  are tag-derived.
- The substitution is scoped to the `[crosspoint]` block, so it can only touch
  that one `version =` line.
- Exactly one leading `v` is stripped; the rest of the tag is used verbatim.

If you ever need to confirm the version that shipped, it's printed in the build
log ("Building CROSSPOINT_VERSION from tag …") and compiled into the firmware.

---

## Adding more assets to the release

Right now only `firmware.bin` is attached. All the other build outputs land in
the **same directory** (`.pio/build/gh_release/`) and are ready to attach. In
the release step of
[`.github/workflows/release.yml`](.github/workflows/release.yml) they are listed
as **real YAML comments directly above the `files:` key** for reference.

To attach one, **copy its path onto its own line inside the `files:` block**
(one real path per line):

```yaml
          #   .pio/build/gh_release/bootloader.bin      <-- reference list above `files:`
          #   .pio/build/gh_release/partitions.bin
          files: |
            .pio/build/gh_release/firmware.bin
            .pio/build/gh_release/bootloader.bin        # <-- copied down to attach it
```

> ⚠️ Do **not** put `#`-commented lines *inside* the `files:` block. It is a
> YAML block scalar, where `#` is literal text — not a comment — so the action
> would treat `# …` as a file pattern and the release step would fail. Keep the
> reference paths as comments *above* `files:`, and only real paths inside it.

Notes:

- `bootloader.bin`, `partitions.bin`, `firmware.elf`, and `firmware.map` are
  produced by the existing `pio run -e gh_release` build — no extra steps
  needed; copying the path in is enough.
- `merged-flash.bin` (a single flashable image) is **not** produced by default.
  To ship it you'd add a step before the release that merges the parts (e.g.
  with `esptool merge_bin`) and writes `merged-flash.bin` into
  `.pio/build/gh_release/`, then add its path to `files:`.
- `fail_on_unmatched_files: true` is set, so if you add a path the build
  doesn't produce, the release step fails loudly rather than publishing a
  release with a missing asset.

---

## Writing changelog-friendly commits

The changelog is generated from your commit messages using the
[Conventional Commits](https://www.conventionalcommits.org/) convention. Good
messages produce a clean, grouped changelog; vague ones land in a generic
"Other" bucket.

Use a `type: description` subject line:

| Prefix      | Section in the changelog   |
| ----------- | -------------------------- |
| `feat:`     | 🚀 Features                |
| `fix:`      | 🐛 Bug Fixes               |
| `perf:`     | ⚡ Performance             |
| `refactor:` | 🚜 Refactor                |
| `docs:`     | 📚 Documentation           |
| `test:`     | 🧪 Testing                 |
| `style:`    | 🎨 Styling                 |
| `revert:`   | ◀️ Revert                  |
| `chore:` / `ci:` / `build:` | ⚙️ Miscellaneous Tasks |

Examples:

```
feat(reader): add per-chapter translation cache
fix(epub): stop crash on malformed spine entries
feat!: drop support for the old XTC layout   # "!" marks a breaking change
```

- Add a **scope** in parentheses (`feat(reader): …`) for a bolded scope tag in
  the entry.
- Mark **breaking changes** with a `!` after the type (`feat!:`) or a
  `BREAKING CHANGE:` footer; they're highlighted and never dropped.
- `chore(release)` and `chore(deps)` commits and merge commits are filtered out
  as noise.

The repo already enforces Conventional Commit **PR titles** via
`action-semantic-pull-request`, so if you squash-merge PRs the squash commit
inherits a well-formed message automatically.

---

## First release (no previous tag)

The changelog normally covers "commits since the previous tag". On the very
**first** tag there is no previous tag, so git-cliff gracefully falls back to
the **full history** instead of failing. Nothing special to do — just tag and
push as usual.

(Cosmetic caveat: on a brand-new repo with a single tag, the very first commit
may be omitted from that first changelog. Subsequent releases are unaffected.)

---

## Pre-releases

Tags like `v1.3.0-rc.1` also match the `v*` trigger and will publish a release.
If you want GitHub to mark them as pre-releases (so they don't show as
"Latest"), edit the release step in the workflow:

```yaml
          prerelease: true
          make_latest: false
```

You can gate this on the tag name if you want it automatic for `-rc` tags.

---

## Re-running / fixing a release

- **Re-running the same tag** updates the existing release in place (assets are
  re-uploaded, the body is overwritten) — it does not error. Handy if a run
  failed partway.
- **Shipped the wrong commit?** Delete the tag locally and remotely, then
  re-tag the correct commit:

  ```sh
  git tag -d v1.2.0
  git push origin :refs/tags/v1.2.0
  # (optionally delete the GitHub Release in the UI)
  git tag -a v1.2.0 -m "CrossLingua v1.2.0"   # on the right commit
  git push origin v1.2.0
  ```

  Prefer cutting a new patch version (`v1.2.1`) over re-using a tag once a
  release has been published and people may have downloaded it.

---

## Troubleshooting

- **Nothing happened after pushing a tag.** The tag must match `v*` (e.g.
  `v1.2.0`, not `1.2.0`). Check the **Actions** tab for a skipped/queued run.
- **Empty or wrong changelog.** Almost always a missing history/tags problem;
  the workflow already uses `fetch-depth: 0`, so this shouldn't happen — if it
  does, confirm the tag was pushed to this repo and earlier tags exist.
- **Release step fails on a missing asset.** You added an asset path to `files:`
  that the build doesn't produce (`fail_on_unmatched_files: true`). Remove that
  line or add the step that produces the file.
- **Build fails at library resolution.** The `open-x4-sdk` submodule must be
  present; the checkout already uses `submodules: recursive`.
