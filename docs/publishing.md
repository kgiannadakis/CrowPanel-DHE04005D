# Publishing Checklist

## Scope

Publish the repository root, including all three firmware trees and new required
headers/tests. They are bundled source, not Git submodules. Nested local `.git`
directories and build caches are not part of the published source.

Do not remove alternate variants, generated protocol bindings, fonts or vendored
libraries simply because the current CrowPanel build does not compile them.
Keep their licenses. No repository-wide formatter or dependency upgrade is
needed for a documentation-only release.

## Before A Commit

1. Run `python tools/check_repository.py` from the root. A C++17 compiler or
   installed PlatformIO RISC-V compiler is required for the complete checks.
2. Build every affected firmware environment. For MeshCore, run
   `platformio run -d meshcore -e dhe04005d-5inch-P4-meshcore`.
   For a release of the complete device, also build Selector and Meshtastic as
   shown in the [installation guide](installation.md#full-dual-boot-installation).
3. Run appropriate emulated and hardware tests from the
   [MeshCore test guide](../meshcore/test/README.md) or
   [Meshtastic host test guide](../meshtastic/test/host/README.md).
4. Review `git status --short`, `git diff --stat` and the complete staged diff.
   Include new required source files and deletions of retired files.
5. Exclude `.pio/`, `logs/`, local `sdcard/` and tile output, private identities,
   credentials, full-flash/NVS backups and editor state. Do not force-add ignored
   files. The hygiene check is a narrow guard, not a comprehensive secret audit;
   review Git history separately before making a private repository public.
6. Update the changelog and document any remaining hardware-test gaps.

The root command also runs the SDK-independent Meshtastic policies. A native
compiler executes those tests; an embedded cross-compiler checks syntax only
and explicitly reports the execution gap. The dependency-backed radio/MQTT,
protobuf and headless UI tests remain separate steps.

CI runs the same root checks on pull requests and pushes to `main`, plus manual
runs. It uses read-only repository permissions and pinned action revisions.
It does not flash devices, build every firmware, create tags or publish releases.
The workflow must still be exercised on GitHub after publication.

## GitHub Upload

The destination is
[kgiannadakis/CrowPanel-DHE04005D](https://github.com/kgiannadakis/CrowPanel-DHE04005D).
Use Git from the repository root, not the web file uploader: the bundled vendor
fonts include individual files larger than the browser upload limit.

```sh
git remote -v
git status --short
python tools/check_repository.py
git diff --check
```

Review the complete working tree before staging, including existing changes
from other work. The firmware projects include required untracked source and
test files until they are staged; committing only modified files is incomplete.
Do not force-add ignored caches, logs or private configuration.

After review, stage the intended changes and inspect `git diff --cached`.
Commit with a descriptive message and push the reviewed branch to `origin`;
do not force-push or rewrite history as part of cleanup. Preparation tools do
not stage, commit or push automatically. Scan history separately when assessing
past exposure; cleaning current files does not remove content from old commits.

## Release Assets

- Build each image from the same reviewed commit and record the environment,
  dependency versions, build identifier and SHA-256 checksum of every asset.
- Preserve matching ELFs privately for crash decoding. Do not distribute device
  backups, saved contacts/settings or unredacted diagnostic captures as firmware.
- Distinguish individual application images from full-installation/merged images.
  Document the expected partition layout and whether this is an update or migration.
- Include a hardware test summary: cold boot, send/receive, large chat history,
  contact persistence, EN/EL/DE editing, maps, Wi-Fi/translation and screen wake.
- Keep the SD emoji atlases in `docs/` as their canonical distribution location.
- Do not label an RC or untested watchdog mitigation as a verified stable release.

Use the [installation guide](installation.md) for flash planning. Publishing a
release never requires erasing a connected device.
