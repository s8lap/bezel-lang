// §7.4 / §22.16 / #2188: contract clauses evaluate at the member's BOUNDARIES
// — `requires` at entry, after argument binding and BEFORE any body guard has
// run (the lowered requires prologue is #2053 slice 4a) — so the body's FINAL
// flow-narrowing state must not admit a nullable PARAMETER access in a clause.
// Here the body's `if s = null then return 0; end;` early-return guard narrows
// `s` to non-null on the fall-through path; without the fix that body-final
// fact leaked into the clause, so `requires(s.Length >= 0)` compiled on a
// `string?` parameter and the lowered prologue then dereffed null on `Len(null)`
// before the body's guard could run (probe-confirmed acceptance on main). The
// clause's unguarded `s.Length` is REJECTED — clause-internal narrowing
// (`requires(s <> null and s.Length >= 0)`) is unaffected (§22.16 "Flow
// narrowing works inside clauses unchanged"). Mirrors the field-set sibling
// readonly_field_narrowing_2116__contract_clause_body_fact_rejected.bzl.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Len(s: string?): int requires(s.Length >= 0) if s = null then return 0; end; return s.Length; end Len; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
