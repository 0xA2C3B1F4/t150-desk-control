# Releases

The first release is being prepared. Until it is published, the examples on
`main` are development configurations. Do not use a future tag as an install
source. Published releases provide tested YAML files and their checksums.

## Version identities

Project releases use `YYYY.M.N`, starting with `2026.11.0` and tag `v2026.11.0`.
The last number increases for another release in the same month. A new month
does not trigger a release. The target version lives in
`components/desk_uart/project_version.json`.

The project version, ESPHome version, protocol library version and controller
firmware identity are separate. The current tested ESPHome is `2026.4.2`;
`library.json` describes the portable C++ library as `0.1.0`. DT02, CL01 and RS01
identify controller patches, not ESPHome releases.

A version in source code is a target version. A published tag and full commit
identify a release. Startup logs include the version, source revision and
`clean`, `dirty` or `unknown` state. Optional diagnostic sensors expose these
values without changing the user's `esphome.project`. A copied component tree
can report its version while its Git revision remains unknown. A fork reports
its own source revision; this is not proof of upstream origin.

## Release contents

Release attachments include the board YAMLs, the existing-device snippet,
optional package YAMLs, firmware identity catalog, install notes, release notes,
`release-manifest.json`, `SHA256SUMS` and a separate validation report. Keep the
YAML files together. The packaged includes and component references are prepared
for this layout and the release tag.

The firmware catalog records exact supported application hashes and existing
transitions. It does not grant permission to install an unknown image. Runtime
hash checks, saved-backup requirements and recovery guards remain authoritative.
No manufacturer firmware, credentials, private logs or compiled ESP images are
included.

`SHA256SUMS` covers the deterministic text assets and manifest, excluding itself.
The validation report records the candidate, workflow revision, test jobs and
run IDs. Its hash is in the release description. Repacking the same source gives
the same text assets; this does not claim reproducible ESPHome binaries.

## Maintainer workflow

Normal development continues through feature branches, PRs and `main`. Prepare
release infrastructure in a separate PR. Keep source changes in one local commit
before pushing when possible; do not rewrite published history.

Before the first release, enable Actions with read-only default tokens and the
required pinned GitHub actions. Require approval for outside fork contributions.
Create a `release` environment restricted to the `main` branch. No additional
reviewer is required for draft creation. Protect `v*` tags against updates and
deletion without bypass actors, while allowing new tags. Enable immutable
releases before publishing. These repository settings are separate from the
workflow files and must be checked through GitHub.

1. Merge the reviewed foundation and select a clean full commit SHA on `main`.
   Check that its target version matches the intended release.
2. Run **Release / candidate-check** from `main` with that version and SHA.
   It runs the complete tests and builds packaged examples using the candidate
   SHA in temporary copies. The original package retains the intended tag.
3. Check the results and perform the stationary Stamp-C3 smoke test on a clean
   build of this candidate. Verify startup, source diagnostics, telemetry and
   existing controls. Earlier R7 hardware results apply to the unchanged
   controller image; they are not a new test of this ESPHome build.
4. After release approval, create an annotated tag on that exact candidate.
   Neither the packer nor the workflow creates or moves tags.
5. Run **Release / tagged-draft** with the same version, SHA and the explicit
   successful candidate-check run ID. This verifies the tag's peeled commit,
   compares packages and tests the literal tag using fresh source caches,
   including the maintenance configurations. An expired candidate artifact
   requires rerunning candidate-check on the same SHA.
6. Inspect the draft attachments, their hashes and the validation report.
   Recheck the tag, protections and completed hardware smoke test. Publish the
   draft as a separate action. Verify that GitHub reports the release immutable.
7. Update `main`'s installation examples to the now-existing stable tag in a small
   documentation PR. When advancing the target version, add its matching version
   heading to `CHANGELOG.md` below Unreleased in the same commit as
   `project_version.json`. Keep the Unreleased section.

The first tag's README may still describe release preparation. Its release page
and attached YAMLs are the installation instructions for that version.

Ordinary PRs may merge during release preparation. Both release stages compare
workflow and release-tool blobs against the candidate. A change to that
infrastructure blocks an older candidate until it is reconsidered. A failed
network request can be retried on the same source. A code fix after tagging
requires a new version and tag.

## Local tools

Use a clean candidate checkout and an output directory outside the repository.
Replace the example SHA with the full candidate commit:

```sh
python tools/release.py validate --source-root . --version 2026.11.0 --commit "$CANDIDATE_SHA"
python tools/release.py package --source-root . --version 2026.11.0 --commit "$CANDIDATE_SHA" --output "$TMPDIR/desk-release"
python tools/release.py verify-tag --source-root . --version 2026.11.0 --commit "$CANDIDATE_SHA"
python tools/check_firmware_catalog.py
```

The package output must not already exist. Validation rejects a dirty source,
wrong version or unexpected commit. `verify-tag` requires an annotated tag and
compares its resolved commit, not the tag object's own SHA.
