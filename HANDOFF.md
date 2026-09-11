# Customer README follow-up

## Goal

Make the public README explain the demo, its benefits, and how to use it.
Keep technical implementation details in the developer guide.

## State

- Branch: codex/docs-followup, based on main 05bf80e.
- Local README rewrite: benefits, requirements, prebuilt flashing, connection,
  dashboard glossary, troubleshooting, source builds, and short end notes.
- Added the official neuralSPOT-X documentation link; previously verified via
  repository Pages settings and HTTP 200.
- Moved SWO diagnostics to the developer guide. Removed repeated transport
  warnings, internal planning text, old board-debug history, and the blanket
  claim that performance figures were modeled rather than measured.
- Preserved license wording and action-critical Apollo330 LP/battery restriction.
- No firmware, dashboard, or release artifact changes.
- Tracking: #90, "Streamline customer README and clarify measurement scope."
  Owner approved publication and merge after required checks pass.

## Validation and next steps

- README relative-link targets and whitespace checks pass; section ordering
  and measurement wording reviewed.
- Visual rendering and hardware testing have not been performed for this edit.
- Publish the PR, verify the required CI matrix, and merge.
- Other worktrees contain unrelated changes; do not overwrite them.
