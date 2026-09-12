# Source comment cleanup

## Goal

Remove migration narratives, stale numeric claims, and internal discussion from
application comments. Preserve contracts, copyright, hardware constraints, and
executable code. Link issue-specific rationale to GitHub.

## State

- Branch: codex/comment-scrub, based on main dd0a31f.
- Local changes span 13 application source files, including main, constants,
  BLE, sensor, timebase, diagnostics, and model adapters.
- Removed TFLM/migration descriptions and obsolete PPG claims. Shortened long
  implementation narratives to invariants and references to existing issues.
- Copyright and license notices, API units, and error contracts retained.
- Source TODOs point to open #71. No closed-issue TODO found in the source sweep.
- Generated/vendor code and technical identifiers are unchanged. The identifier
  g_as7058_profile_legacy_default remains to keep this pass comment-only.
- Host tests: 12 passed. Non-comment token comparison passed for edited C/C++
  files, rechecked before publication. No hardware test or firmware-image parity
  claim. Source line changes can affect debug metadata.
- Tracking: #92, "Clean up source comments for public readability."
  Owner approved issue and PR creation, not merge.

## Findings and scope limits

BLE task-handle teardown has a documented concurrent-reader race. The scrub
retains a short warning and #19 context; a dedicated bug draft should capture
that unresolved race before any behavioral fix. Do not silently remove it.
Tool scripts were keyword-scanned, not exhaustively rewritten. Their runtime
TODO(verify) strings are fail-closed diagnostics, not deferred code comments.
Private driver sources, generated modules, and archived benchmark data excluded.

## Next steps

- Publish the cleanup PR and review hosted checks; leave open for owner review.
- Treat BLE lifetime behavior as a separate issue and code change.
