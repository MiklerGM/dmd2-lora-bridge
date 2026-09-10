---
name: test-value
description: >
  Decide whether automated tests should be added, changed, merged, redone, or
  removed. Use when planning, adding, modifying, or reviewing tests, including
  AI-generated test diffs and deciding whether regression coverage is warranted.
  Do not use merely to run unchanged tests.
---

# Test Value

Optimize for confidence from unique defect-detection signal, not coverage
percentage or test count. No new test is a valid and often correct outcome.

## Before editing: value gate

Before creating or materially changing each test case, identify:

1. The observable behavior or public contract it protects.
2. A plausible production defect that would violate that behavior.
3. The exact assertion or invariant that would fail for that defect.
4. Why existing tests do not already detect the same defect.
5. Why the maintenance cost is justified by the risk.

If any answer is missing, do not add or change the test. Do not infer value from
an uncovered line, branch, method, exception, or public function.

## Test budget

Prefer the smallest non-redundant test set. For a small change, expect zero to
three new test cases. This is a friction gate, not a hard limit: exceed it only
after identifying the distinct material risk protected by each additional case.
Parameterized rows for one behavior count as one case for this decision.

Use parameterized or table-driven tests for equivalent cases. Do not
mechanically split one scenario because it has multiple assertions; multiple
assertions are appropriate when they jointly prove one observable outcome.

## Low-value subjects

Do not add tests whose primary purpose is to preserve:

- private methods, internal branches, helper calls, or incidental call order;
- trivial getters, constructors, constants, forwarding methods, or dependency
  injection wiring in isolation;
- behavior supplied by the language, framework, ORM, router, or library rather
  than behavior owned by this repository;
- mocks, stubs, fixtures, test IDs, Jest or visual snapshot artifacts, or other
  test machinery as ends in themselves;
- exact internal text, class names, DOM structure, or serialization shape unless
  it is explicitly user-visible or externally consumed;
- the existence or absence of a symbol or source-code line;
- a coverage percentage.

Repository-specific configuration, wiring, persistence, transactions,
serialization, permissions, and interaction order can be valid contracts. Test
their resulting behavior or state; assert calls or order only when the
interaction itself is externally meaningful or transactional.

A behavior-preserving refactor should not require test changes.

## Assertions and oracles

Assert observable outputs, persisted state, contractual events, responses,
permissions, errors, or user-visible behavior.

Derive expected values independently. Production helpers and builders may set up
inputs, but do not use the same production algorithm, serializer, or default
registry to compute both the actual and expected result.

Avoid truthiness, non-null, non-empty, and "does not throw" assertions unless
that exact property is the contract.

## Mocks and test level

Prefer real internal components. Mock boundaries that are external, slow,
destructive, non-deterministic, rate-limited, or otherwise unsuitable for the
test. Assert the resulting behavior, not merely that a mock was called.

If mock setup is larger than the behavior under test, move to a more appropriate
integration boundary or do not add the test.

Use the cheapest test level that still crosses the boundary where the plausible
defect can occur. Do not replace a wiring, persistence, serialization,
transaction, or integration risk with a unit test that mocks away that risk.

## Bug fixes

Prefer one regression test that:

1. reproduces the reported behavior;
2. fails against the pre-fix behavior for the expected reason;
3. passes after the fix; and
4. remains valid under a different internal implementation.

If it would pass before the fix, it is not the regression test. Add more cases
only for materially different failure modes.

## Before finalizing: changed-test review

Classify every added or materially modified test case:

- **KEEP** — uniquely detects a meaningful plausible defect.
- **MERGE** — valuable signal duplicates or fragments another test.
- **REDO** — valuable behavior, but the test has a weak oracle, wrong level,
  unrealistic setup, or implementation coupling.
- **DELETE** — no unique meaningful defect signal or unjustified maintenance
  cost.

Use **DELETE** for newly added tests that test machinery or structure, duplicate
another test's signal, survive the claimed regression, or cannot name a
realistic bug they catch. For a pre-existing test, **DELETE** normally means
reverting the unjustified changed hunk, not deleting the whole test. Do not
remove unrelated test debt unless the task explicitly includes it.

In a review-only task, report classifications without editing. When edits are
authorized, apply justified merges, redesigns, and deletions, then re-review:
every remaining changed test should be **KEEP**. Run the smallest relevant test
command. Add a replacement only when material behavior remains unprotected.
Report final counts for test cases added, merged, redone, and deleted, including
zeros.

## Independent review for non-trivial diffs

When a changed-test diff is substantial or its value is uncertain, delegate one
independent read-only review. Do not delegate again if you are that reviewer.
Use this prompt, replacing `<diff>` with the actual comparison:

```text
Use $test-value to review only test cases added or modified in <diff>.

Inspect production code and existing tests as needed to judge observable
behavior, plausible regressions, duplication, refactor resistance, and test
level. Do not broaden into an unrelated production-code review.

Return one evidence-backed verdict per changed test case:
KEEP
MERGE with <test>
REDO
DELETE

For a modified pre-existing test, DELETE means revert the unjustified changed
hunk unless deleting the whole test is explicitly in scope. Do not edit files,
delegate further, or add replacement tests. Identify any material behavior that
would remain unprotected and suggest the smallest relevant test command. The
parent agent owns edits and validation. "No replacement needed" is valid.
```
