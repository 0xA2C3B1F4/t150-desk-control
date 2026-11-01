# Releases

Until the first release, use the development configurations on `main`. Published
releases provide YAML files pinned to their tag, checksums and validation results.
Use tags for installation only after their release is published.

## Version identities

Project releases use `YYYY.M.N`, starting with `2026.11.0` and tag `v2026.11.0`.
The last number increases for another release in the same month. The target
version lives in `components/desk_uart/project_version.json`; the published tag
and full commit identify a release. A new month does not trigger a release.

Tested ESPHome `2026.10.1`, the protocol library's `0.1.0` and controller patches DT02,
CL01 and RS01 have separate versions. Startup logs and optional
[source diagnostics](esphome.md#source-diagnostics) report the project version,
revision and source state. Copied components may have an unknown revision;
a fork's revision identifies its own source, not upstream origin.

## Release contents

Keep the downloaded board, existing-device and optional package YAMLs together.
Their includes use this flat layout, and component references use the release tag.
Other attachments are the firmware identity catalog, install and release notes,
`release-manifest.json`, `SHA256SUMS` and a validation report. Manufacturer firmware,
credentials, private logs and compiled ESP images are excluded.

The catalog records supported hashes and transitions. Runtime hash checks,
backup requirements and recovery guards still determine which images can be
installed. `SHA256SUMS` covers the repeatable text assets and manifest. The
validation report records source and workflow revisions, test jobs and run IDs;
its hash is in the release description. ESPHome binary reproducibility is not
claimed.

## Maintainer workflow

Select releases from `main` after normal feature-branch PRs. Before releasing,
verify the repository settings: Actions enabled with read-only default tokens,
SHA-pinned actions and approval for outside fork runs; a `release` environment
restricted to `main`; and protection against updating or deleting `v*` tags,
without bypass actors. Allow new tags and enable immutable releases before
publishing. Workflow files do not configure these settings.

1. Select a clean full commit SHA on `main` whose target version matches the
   intended release.
2. Run **Release / candidate-check** from `main` with that version and SHA.
   It runs the full test suite and builds packaged examples using the SHA in
   temporary copies; release assets retain the intended tag.
3. Check the results, then test a clean build on a stationary Stamp-C3: startup,
   source diagnostics, telemetry and existing controls. Earlier R7 hardware results
   cover the unchanged controller image, not this ESPHome build.
4. After approval, create an annotated tag on the exact candidate. The tools
   never create or move tags.
5. Run **Release / tagged-draft** with the same version, SHA and successful
   candidate-check run ID. It checks the tag's peeled commit, compares packages
   and tests the literal tag in fresh source caches, including maintenance
   configurations. If the artifact expired, rerun candidate-check on the same SHA.
6. Check the draft's attachments, hashes and validation report, the tag and its
   protections, and the completed hardware test. Publish separately, then verify
   GitHub reports the release immutable.
7. Update `main`'s installation examples to the published tag in a documentation
   PR. When advancing `project_version.json`, add the matching version heading
   below Unreleased in `CHANGELOG.md` in the same commit. Keep Unreleased.

The first tag's README may describe release preparation. Use its release page
and attached YAMLs for installation.

Other PRs may merge during preparation. Release stages compare workflow and
release-tool blobs with the candidate; infrastructure changes block an older
candidate until reconsidered. Retry network failures on the same source.
A code fix after tagging needs a new version and tag.

## Local tools

Use a clean candidate checkout and set `CANDIDATE_SHA` to its full commit. Choose
a new output directory outside the repository:

```sh
python tools/release.py validate --source-root . --version 2026.11.0 --commit "$CANDIDATE_SHA"
python tools/release.py package --source-root . --version 2026.11.0 --commit "$CANDIDATE_SHA" --output "$TMPDIR/desk-release"
python tools/release.py verify-tag --source-root . --version 2026.11.0 --commit "$CANDIDATE_SHA"
python tools/check_firmware_catalog.py
```

Validation rejects dirty sources and version or commit mismatches. Package checks
reject unexpected assets. Tag verification requires an annotated tag and compares
its resolved commit, not the tag object's SHA.
