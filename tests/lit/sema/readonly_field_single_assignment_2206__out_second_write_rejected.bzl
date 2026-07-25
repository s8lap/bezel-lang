// §22.8 (v0.226) single-assignment of readonly fields (#2206, route A).
//
// RESIDUAL after #2187 (class direct-assignment family) and #2207 (record
// ctor): the once-check (checkReadonlyOnceness) fired only on
// AssignmentStmt/IncrementStmt, not on `out`/`ref` argument writes. So a
// readonly field assigned by `:=` could be written a SECOND time through an
// `out` argument without diagnosis — the same finality break the §7.4/#2116
// narrowing anchor is sound only against (an observed non-null readonly value
// must be final; the `out` write could null it after the fact rode a closure).
// This is the exact repro from the issue. Now rejected: visitCallArg feeds
// every `ref`/`out` operand through checkReadonlyOnceness, so the `out _f`
// here is the second write of `_f`.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _f: string?; public constructor() _f := "a"; SetIt(out _f); end constructor; destructor end destructor; public method SetIt(out x: string?) x := null; end SetIt; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)
