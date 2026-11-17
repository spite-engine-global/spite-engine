# SDK qualification and promotion

Every publisher output is an immutable unverified candidate, even if local checks pass.
The manifest describes the candidate; `qualification.json` and the evidence directories in
the promotion bundle describe its distributable state. Keep them with the archive. The SDK
launcher warns because the embedded manifest cannot know about a later external promotion.
No Linux promotion is implemented: Linux archives remain candidates until Linux-specific
qualification exists; a Windows Iris Xe receipt does not qualify them.

On the Windows build machine, publish with `--verify-consumer` and without
`--skip-runtime-smoke`. Prepare the floor project from that exact zip:

```
python scripts/QualifySdk.py prepare path/to/SPITE-SDK-<version>-windows-x86_64-UNVERIFIED.zip path/to/prepared
```

Transfer the zip and prepared directory unchanged to the floor machine. It needs Python,
PowerShell 7 and this checkout's `scripts/RunEngine.ps1` and its script dependencies;
no compiler or project build runs there. Run:

```
python scripts/QualifySdk.py qualify path/to/candidate.zip path/to/prepared path/to/floor-evidence --run-engine path/to/scripts/RunEngine.ps1
```

The tool creates a fresh extraction, checks Windows PowerShell 5.1 and the shipped launcher,
then runs the editor, game, built project and pipeline qualification with bounded run locks.
The pipeline host must emit `SPITE pipeline qualification: PASS` only after all shipped
pipelines succeed, and exit nonzero on any failure. A missing marker fails the run. GPU IDs,
raw driver value, exit status, source head, archive hash and log hashes are retained; inspect
the logs before handing the floor evidence to the publisher. The floor is Intel 8086:46A6.

`SpiteEditor --qualify-pipelines` audits shader coverage and creates the supported pipelines
without a scene. Unsupported mesh-tier recipes are reported explicitly; the fallback recipes
still run. A fatal driver failure stops the run immediately. For developer fault checks,
Debug hosts accept `SPITE_FAIL_PIPELINE_AT=N` to reject the Nth startup pipeline; unset it
for the uninjected control.

On the publication machine:

```
python scripts/PromoteSdk.py path/to/candidate.zip path/to/candidate-checks.json path/to/floor-evidence/receipt.json path/to/new-promotion-directory
```

Promotion verifies the recorded identity, logs and required runs, and copies the exact archive
bytes (named `SPITE-SDK-<version>-windows-x86_64.zip`), checksum, the `spite-release.json`
release feed and evidence into a new directory. Evidence is a manual owner handoff, not a
cryptographic signature or a defense against deliberate fabrication. A changed candidate
requires a fresh preparation and qualification. Failures leave diagnostic logs; choose a new
output directory for a retry.
