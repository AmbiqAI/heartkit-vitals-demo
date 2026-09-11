# Public repository readiness

## Goal

Prepare metadata and documentation for public availability. Do not change
visibility or rewrite history in this task.

## State

- Base: main `71232cd`, following PR #85 and firmware v5.2.0.
- Branch: `codex/public-readiness`. Tracking: issue #86, following issue #41.
  Owner approved issue creation and cleanup PR publication.
- Cleanup PR: https://github.com/AmbiqAI/heartkit-vitals-demo/pull/87.
- Owner approved inclusion of AS7058 presets and generated profiles.
- Gitleaks 8.30.1 scanned all fetched refs with `--all --full-history`,
  full redaction, and inline allow comments ignored on September 11, 2026:
  171 commits, approximately 244 MB, zero findings. Repository is not shallow.
- Independent content review found stale FAE instructions and operational
  clutter in this handoff. Cleanup is prepared with contribution/security
  guidance, CODEOWNERS, templates, and mixed-license clarification.
- Hardware coverage remains in [release validation](docs/release-v5.2.0-validation.md).
  No firmware change or new hardware test is part of this documentation task.

Validation: all 12 standalone host tests pass; `git diff --check` passes.
Independent patch review found no blocking inaccuracies. Before public promotion,
enable and verify private vulnerability reporting or supply an approved public
security contact; an outside researcher may not have an Ambiq representative.

## Next steps

- Review PR #87 and hosted checks. Keep #86 open for the remaining
  public-promotion gates.
- Owner decision: retain or redact historical attribution trailers and internal
  policy discussion in issue #41. No detected credential justifies blanket
  history rewriting; a git rewrite would not remove issues or Actions content.
- Confirm security/reporting settings and required CI before public promotion.
  Source builds still require private dependency access.

## Coverage limits

Content review sampled historical discussions and one merged-main Actions log,
not every historical log or release archive. Gitleaks covers fetched git history,
not discussions, remote-only refs, or release assets. Generated AOT modules keep
their hardware-restricted licenses; root BSD terms do not override them.
