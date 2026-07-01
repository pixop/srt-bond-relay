# AGENTS.md

This file defines the baseline engineering standards for contributors and AI agents working in `srt-bond-relay`.

## Mission And Priorities

Prioritize, in this order:

1. Correctness and reliability in live streaming behavior.
2. Backward-compatible operator experience (CLI behavior, metrics shape, and logs).
3. Maintainability and readability over cleverness.
4. Test coverage for behavior changes.
5. Performance improvements that preserve correctness and observability.

## Repository Conventions

- Language/runtime: C++17 with CMake.
- Keep changes minimal and scoped; avoid drive-by refactors.
- Match existing naming and layout patterns (`src/`, `include/srtrelay/`, `tests/`).
- Prefer small, composable functions over large multi-purpose blocks.
- Do not silently change defaults, metric names, or CLI semantics.
- Do not reinvent mature, well-supported solutions when a dependency is safer and simpler.

## Coding Style Consistency

- Follow existing local style in touched files (brace placement, spacing, and naming).
- Prefer explicit types at module boundaries and public APIs.
- Keep `const` correctness and immutable-by-default local variables.
- Keep includes minimal and grouped consistently with surrounding code.
- Avoid hidden control flow and side effects.
- Comments should explain intent or invariants, not restate obvious code.

## Error Handling And Reliability

- Fail fast on invalid configuration; provide actionable error messages.
- Handle partial failure paths explicitly (input/output reconnect, timeouts, degraded states).
- Never swallow exceptions/errors silently.
- Preserve causality and structured logging behavior for production debugging.
- Keep timeout/retry logic deterministic and testable.
- Treat shutdown and cleanup paths as first-class behavior.

## Metrics And Observability

- Preserve metric compatibility unless a deliberate breaking change is requested.
- When adding/changing metrics, update:
  - `include/srtrelay/metrics.hpp`
  - `src/metrics.cpp`
  - relevant tests (for example `tests/metrics_compat_test.cpp`)
- Keep metric labels and cardinality bounded and stable.
- Ensure important state transitions are observable via logs and/or metrics.

## Testability Requirements

- Every behavior change should include or update tests in `tests/`.
- Prefer focused unit-style coverage for parsing/state logic and deterministic runtime behavior.
- Add regression tests for bug fixes.
- Keep tests deterministic (no flaky timing assumptions, no external network dependencies when avoidable).

## Dependency Policy (Pragmatic, Not Purist)

- Prefer proven third-party libraries over custom implementations for non-core problems.
- Header-only dependencies are welcome when they improve delivery speed and reduce operational risk.
- Select dependencies based on maintenance health, adoption, license fit, and API stability.
- Keep dependency surface area minimal: choose focused libraries over large frameworks.
- Document why a new dependency is introduced and what internal code it replaces/simplifies.
- Pin versions intentionally and update with explicit compatibility checks.
- For critical paths, validate runtime/latency impact before and after adoption.

## Validation Before Hand-Off

Run these from repository root:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

If a change affects containers, also validate the relevant Docker build path.

## Change Discipline

- Keep commits logically atomic and easy to review.
- Document non-obvious trade-offs in PR description or commit message.
- Update `README.md` when behavior, flags, or operator workflows change.
- Prefer introducing seams for future improvements instead of broad rewrites.

## AI Agent Guardrails

- Do not rewrite large files unless necessary for the requested change.
- Do not introduce new dependencies without clear justification.
- Avoid speculative abstractions not needed by current behavior.
- Keep production mindset: observable, recoverable, and safe-by-default.
