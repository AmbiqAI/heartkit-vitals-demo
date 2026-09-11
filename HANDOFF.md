# Customer documentation polish

## Goal and scope

Use Vital Sign Monitoring as the demo name, powered by heartKIT and accelerated
with heliaAOT. Keep technical identifiers, firmware behavior, and historical
benchmark evidence unchanged.

## State

- Branch: codex/customer-docs, based on origin/main db3636f.
- Local edits: README overview and quick start, developer guide, FAE runbook,
  and asset-provenance brand capitalization.
- Quick start A (prebuilt firmware) precedes Quick start B (source build).
- README tile glossary is a short customer-facing table. Implementation
  details are in the developer guide; historical captures there are explicitly
  identified as build-specific records rather than current expectations.
- American English spelling standardized in README, asset provenance, and
  streaming design prose; license texts and code identifiers are unchanged.
- Verified the heartKIT and heliaAOT documentation homepages and v5.2.0 release
  asset names. README download instructions now use the published v520 package.
- Tracking: issue #88, "Polish customer-facing naming and overview documentation."
  Owner approved PR creation and merge after final review and passing checks.
- Final diff review corrected unavailable-value wording to distinguish model
  efficiency from aggregate AI Throughput. No firmware changes are included.
- Paired TileIO checkout was inspected but not edited.

## Next steps

- Publish the documentation PR, verify all five required checks, and merge.
- No hardware testing is required for these prose changes; no hardware result
  is claimed. Markdown has not been visually rendered in this task.

## Context

Public-readiness PR #87 is merged. The repository is public and GitHub security
settings were verified in the preceding task. Do not revert those settings or
rewrite history. Other worktrees contain unrelated local changes.
