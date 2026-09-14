# Item11 timestamp correction handoff

Status: blocked-manual. Local defect fixed; fresh timeline/image audit pending. Original native/clean-host/UI/device/MSI-install/signing and protected-check blockers persist.

## Measured defect and correction

The diagnostic saved pixels and timestamp from the same VideoFrame. Exact owned MP4 container frame0 has PTS0.000000. Actual GStreamer video segment starts66666666ns with time0 and first raw buffer PTS66666666ns; audio segment starts21333333ns with time0 and first raw PCM PTS21333333ns. FileSource incorrectly exposed raw decoder PTS as media time for both tracks. This was a product timestamp defect rather than diagnostic bookkeeping.

`FileSource.cpp` now converts both samples with `gst_segment_to_stream_time`; invalid conversions keep the existing decode-failure path. No source/codec fallback. New `StreamingFileTimelineTests.cpp` generates a real H264/AAC MP4 and compares video/PCM at media0 and500ms seek. All four rows failed with the exact offsets before fixing production; all pass afterward. Every row independently faults the corresponding production timestamp assignment back to rawPTS, fails as intended and passes after restoration. Existing protected tests unchanged; probe unchanged.

New totals:54 C++ binaries plus Python package suite,58 CTest registrations (52 quick/6 extended). ON/OFF all-consumer/app/core/daemon builds exit0. Full58:55 pass/3 fail95.85s. Foreground/audio are original failures. The StreamingMediaTests failure has no per-case Qt output; one focused full rerun captures23 passes20.738s. Do not infer the earlier cause from that pass; retain unexplained failure for item12. No repeated-until-green validation.

## Fresh proof audit batches

Supply a fresh reviewer only success criterion and absolute artifact paths. New timeline cycles and new image/package evidence are pending fresh audit. Parent already passed historical22 insurance cycles.

### Measured container and decoder segments establish the video/audio offsets on the actual owned fixture; faithful four-row regression fails before production fix and passes after

- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/timeline-container-diagnostic-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/timeline-gst-diagnostic-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/timeline-gst-audio-diagnostic-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/timeline-red-command-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/timeline-red-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/timeline-green-command-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/timeline-green-01.txt`

### Four assertions each have initial green, one intended production fault failure, restoration and pass

- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/timeline-green-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/timeline-insurance-01`

### Affected ON/OFF builds and both package formats exit0; final payload manifests match101/102 runtime hashes and current operational docs

- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/timeline-build-on-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/timeline-build-off-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/package-timeline-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/package-msi-timeline-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/archive-timeline-verify-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/msi-timeline-payload-verify-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/packaged-timeline-docs-verify-01.txt`

### Corrected private package starts isolated help and performs real file decoding/PCM, TLS, separate-process media and packaged scanner/module loading

- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/clean-runtime-timeline-01`

### Saved decoded file image visually matches the owned reference frame at media time0; image manifest and captured reference command identify both timestamps/hashes; transported outputs show red and blue

- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/clean-runtime-timeline-01/decoded-file.png`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/clean-runtime-timeline-01/owned-reference-frame-0ns.png`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/clean-runtime-timeline-01/image-manifest.json`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/clean-runtime-timeline-01/reference-command.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/clean-runtime-timeline-01/decoded-media/decoded-red.png`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/clean-runtime-timeline-01/decoded-media/decoded-blue.png`

### Full suite failure and focused followup remain accurately scoped; final quick runs after documentation

- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/full-timeline-command-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/full-timeline-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/full-timeline-cases-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/media-timeline-diagnosis-command-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/media-timeline-diagnosis-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/quick-timeline-command-01.txt`
- `C:/Work/projects/deskflow-fch/temp/proof-of-work/worklist-2026-09-13-1250/item-11/quick-timeline-01.txt`

## Current artifacts

- Portable: `C:/Work/projects/deskflow-fch/temp/package-timeline-11-01/deskflow-1.26.0.9999-win-x64-portable.7z`
- Portable SHA256: `4a9f1295c4edd73488ea3a0a5562d4f46341f08253e48e014834565d378906e3`
- MSI: `C:/Work/projects/deskflow-fch/temp/package-msi-timeline-11-01/deskflow-1.26.0.9999-win-x64.msi`
- MSI SHA256: `8539ed497ce2ce2360abccd179f623f6111055caa5f87d18fe0a352531209f05`
- Product extraction: `C:/Work/projects/deskflow-fch/temp/package-extracted-timeline-11-01/deskflow-1.26.0.9999-win-x64-portable`
- Validation-only copy: `C:/Work/projects/deskflow-fch/temp/package-validation-timeline-11-01`; includes added probes/tests and must never be distributed.
- Proof scripts: `C:/Work/projects/deskflow-fch/temp/insurance-timeline-11.py`, `C:/Work/projects/deskflow-fch/temp/create-timeline-image-proof-11.py`, `C:/Work/projects/deskflow-fch/temp/verify-archive-timeline-11.py`, `C:/Work/projects/deskflow-fch/temp/verify-msi-timeline-11.py`, `C:/Work/projects/deskflow-fch/temp/validate-package-timeline-11.py` and its headless wrapper.

The corrected file diagnostic reports6 video frames and8 PCM blocks/8192 stereo frames. The saved PNG media time0 matches FFprobe reference0.000000; mean absolute RGB differences are approximately0.56/1.53/0.65 from separate decoder/conversion paths. No audible output/device capture or physical A/V claim.

## Exclusions and missing checks

`timeline-segments-02.txt` is a failed diagnostic setup with malformed source paths, not segment evidence. The two original gst diagnostic logs above contain the measured segments and successful exits. The original handoff indexed the failed setup incorrectly; the blind reviewer identified this and the index is corrected.

Keep `clean-runtime-final-02/decoded-file.png` and `owned-reference-frame-66666666ns.png` immutable. This original pair is explicitly rejected; its metadata/reference do not establish matching content time. Earlier packages contain the raw-PTS defect and are superseded. Historical runtime transport/factory/module results remain scoped to their actual runs; do not treat rejected timestamp proof as accepted.

`msi-timeline-extract-01.txt` retains WiX's source-decompiler ErrorTable crash after file extraction. Successful independent read-only MSI database/payload comparison is the extraction verification; no MSI installer or administrative install was run. Native platforms lack compilation/test insurance/runtime proof. Clean-host MainWindow/capture/audio/peer acceptance, MSI installation, signing and fresh new proof audit remain missing. Whole-project full/quick checks remain blocked by protected/prerequisite failures. PROJECT.md and supplied instructions must be reread before final quick; no item12 execution in this child.
