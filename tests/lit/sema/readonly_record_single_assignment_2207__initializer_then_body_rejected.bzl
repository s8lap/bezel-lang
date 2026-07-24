// §22.8 (v0.226) single-assignment of readonly fields on RECORDS (#2207).
//
// A record readonly field with a FIELD INITIALIZER is assigned as step 2 of a
// non-chaining record ctor, before the body (§22.8 — "A field declared in a
// class or record may include an initializer"). Re-assigning it in the body
// is therefore a SECOND assignment — the same finality-breaking hole as the
// class case (#2187). The fix's `readonlyMaybeAssigned_` seed at body entry
// (the `readonlyInitialized` loop in runRecordConstructor) rejects this.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; record R public readonly _f: string? := "abc"; public constructor() _f := null; end constructor; public static method Main(): int return 0; end Main; end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)