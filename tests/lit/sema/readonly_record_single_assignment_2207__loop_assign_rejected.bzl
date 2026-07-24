// §22.8 (v0.226) single-assignment of readonly fields on RECORDS (#2207).
//
// A readonly field assigned inside a loop body on a record could recur on a
// later iteration — a potential re-assignment the compiler cannot rule out
// (§7.4 narrowing is carved out of the construction window, so no per-field
// flow fact bounds the loop). §22.8's single-assignment rule rejects it.
// The same inLoopDepth_ check #2187 added for classes fires for records now
// that the roCtorMode_ gate is enabled in runRecordConstructor.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; record R public readonly _f: int; public constructor() for var i in 0..3 do _f := i; end; end constructor; public static method Main(): int return 0; end Main; end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` inside a loop
// CHECK-SAME: (§22.8 single-assignment)