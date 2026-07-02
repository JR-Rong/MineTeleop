# Target Host Duplicate Weak Network Profile Validation Implementation Plan

**Goal:** Reject target-host `network.weak.matrix` archives where a documented weak-network profile appears more than once.

**Architecture:** The validator already requires the documented weak-network profile set and exact apply/clear command counts. Add duplicate `profile=` detection before set-based missing/unexpected checks so repeated profile evidence cannot be hidden.

## Steps

- [x] **Step 1: Add RED archive test**
  - Generate the documented matrix and append a duplicate `profile=...` line without extra apply/clear commands.
  - Expected failure reason: `weak_network_matrix_duplicate_profiles`.
  - Confirm the focused test currently fails because the archive is accepted.
  - RED result: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_weak_network_profile` failed with `AssertionError: 0 != 2`.

- [x] **Step 2: Add duplicate profile validation**
  - Extract non-empty `profile=` names in order.
  - Reuse `_duplicate_values` and fail on the first duplicate profile.
  - GREEN result: focused test passed.

- [x] **Step 3: Validate**
  - Run the focused test.
  - Run adjacent weak-network archive tests.
  - Run the full `ContainerTemplateTests` surface.
  - Run py_compile, diff check, and `scripts/check.py`.
  - Focused test: PASS.
  - Adjacent weak-network archive tests: 5 passed after correcting the module path.
  - `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`: 125 passed.
  - `python3 -m py_compile mine_teleop/deployment_validation.py tests/test_deployment_templates.py`: PASS.
  - `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-lane-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-probe-scenario-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-weak-network-profile-validation.md`: PASS.
  - `python3 scripts/check.py`: 456 passed.

## Review Lanes

- Reuse: Reuse `_duplicate_values` and existing `_weak_network_matrix_failure` payload style.
- Quality: Preserve missing, unexpected, and command-count checks.
- Efficiency: Reuse already-read stdout text lines; only in-memory counting.

## Post-Implementation Review

- Reuse: Reused the duplicate-value helper introduced for hardware probes and the existing weak-network failure shape.
- Quality: Duplicate profile evidence is rejected before missing/unexpected profile checks, producing a precise reason.
- Efficiency: Adds only in-memory counting over already-read stdout text.
- Regression risk: Adjacent warning, unexpected profile, and weak-network baseline tests still pass.
