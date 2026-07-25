// §22.8 (v0.226) single-assignment of readonly fields on RECORDS (#2206 route A).
//
// The fix routes `ref`/`out` operands through checkReadonlyOnceness inside
// `visitCallArg`, which runs wherever `roCtorMode_` is set — class ctors
// (#2187), static ctors, AND record ctors (#2207's runRecordConstructor). So
// the `out` second-write residual is closed on records too. A record readonly
// field assigned by `:=` then written through an `out` arg is rejected.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; record R public readonly _f: string?; public constructor() _f := "a"; SetIt(out _f); end constructor; public method SetIt(out x: string?) x := null; end SetIt; public static method Main(): int return 0; end Main; end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)
