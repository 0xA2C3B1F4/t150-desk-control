# Releases

Install from a [published release](https://github.com/0xA2C3B1F4/t150-desk-control/releases).
Each release provides a Stamp-C3 factory image, YAML files pinned to its tag,
checksums and validation results. The configurations on `main` follow development
sources.
Use tags for installation only after their release is published.

## Version identities

Project releases use `YYYY.M.N`, starting with `2026.11.0` and tag `v2026.11.0`.
The last number increases for another release in the same month. The target
version lives in `components/desk_uart/project_version.json`; the published tag
and full commit identify a release. A new month does not trigger a release.

The maintenance page shows the project target version for the controller image;
the version is not embedded in the image. Its SHA-256 identifies the actual
bytes. Historical patch IDs remain in the compatibility catalog.
ESPHome `2026.10.1` and the protocol library's `0.1.0` have separate versions. Startup logs and optional
[source diagnostics](esphome.md#source-diagnostics) report the project version,
revision and source state. Copied components may have an unknown revision;
a fork's revision identifies its own source, not upstream origin.

## Release contents

Keep the downloaded board, existing-device and optional package YAMLs together.
Their includes use this flat layout, and component references use the release tag.
The Stamp-C3 image is `t150-stamp-c3-<version>.factory.bin`; follow
[binary installation](binary-install.md). Other attachments are the firmware
identity catalog, install and release notes, `release-manifest.json`,
`SHA256SUMS` and a validation report. Manufacturer firmware images, credentials
and private logs are excluded. There is no OTA or maintenance binary.

The catalog identifies the current controller image and supported historical
images. The release manifest binds the current image ID and SHA-256 to the
release version and source commit. Runtime hash checks,
backup requirements and recovery guards still determine which images can be
installed. The manifest also identifies the factory image's source SHA, target,
size and SHA-256. `SHA256SUMS` covers the text assets, manifest and factory image. The
validation report records source and workflow revisions, test jobs and run IDs;
its hash is in the release description. ESPHome binary reproducibility is not
claimed.

## Maintainer workflow

Select releases from `main` after normal feature-branch PRs. Before releasing,
verify the repository settings: Actions enabled with read-only default tokens,
SHA-pinned actions and approval for outside fork runs; a `release` environment
restricted to `main`; and protection against updating or deleting `v*` tags,
without bypass actors. Allow new tags and enable immutable releases before
publishing. Require the `ci-result` status check for PRs into `main`; individual
matrix jobs may be skipped for documentation changes. Workflow files do not
configure these settings.

1. Select a clean full commit SHA on `main` whose target version matches the
   intended release.
2. Run **Release / candidate-check** from `main` with that version and SHA.
   It runs the full test suite, validates packaged examples using the SHA and
   builds the factory image once. Release YAML assets retain the intended tag.
3. Download that run's artifact and test its exact factory image: USB/Wi-Fi
   setup, web, Home Assistant, telemetry and preset/Stop controls. Record the
   run ID, artifact ID, source SHA and binary hash with the results. When
   restoring a personal YAML build from the same source, check startup, source
   diagnostics and telemetry. Existing controller tests retain their original scope.
4. After approval, create an annotated tag on the exact candidate. The tools
   never create or move tags.
5. Run **Release / tagged-draft** with the same version, SHA and successful
   candidate-check run ID. It verifies the tag and candidate artifact, retains
   the tested binary and tests the literal tag in fresh source caches, including
   maintenance configurations. It does not rebuild the published binary.
6. Check the draft's attachments, hashes and validation report against the
   hardware approval. Verify the tag and its protections, publish separately,
   then verify GitHub reports the release immutable.
7. Update `main`'s installation examples to the published tag in a documentation
   PR. When advancing `project_version.json`, add the matching version heading
   below Unreleased in `CHANGELOG.md` in the same commit. Keep Unreleased.

The first tag's README may describe release preparation. Use its release page
and attached YAMLs for installation.

Other PRs may merge during preparation. Release stages compare workflow,
release-tool and CI-helper blobs with the candidate. Infrastructure changes block
an older candidate until reconsidered. Candidate artifacts expire after 14 days.
An expired artifact or a new build requires acceptance of the new binary before
publication, even for the same source SHA. A retained local copy is evidence,
not a substitute artifact for the workflow. A code fix after tagging needs a
new version and tag.

## Local tools

Use a clean candidate checkout and set `CANDIDATE_SHA` to its full commit. Choose
a new output directory outside the repository:

```sh
python tools/release.py validate --source-root . --version 2026.11.0 --commit "$CANDIDATE_SHA"
python tools/release_binary.py build --source-root . --version 2026.11.0 --commit "$CANDIDATE_SHA" --output "$TMPDIR/desk-binary" --build-root "$TMPDIR/desk-binary-build"
python tools/release.py package --source-root . --version 2026.11.0 --commit "$CANDIDATE_SHA" --binary-build "$TMPDIR/desk-binary" --output "$TMPDIR/desk-release"
python tools/release.py verify-tag --source-root . --version 2026.11.0 --commit "$CANDIDATE_SHA"
python tools/check_firmware_catalog.py
```

Validation rejects dirty sources and version or commit mismatches. Package checks
reject unexpected assets. Tag verification requires an annotated tag and compares
its resolved commit, not the tag object's SHA.
