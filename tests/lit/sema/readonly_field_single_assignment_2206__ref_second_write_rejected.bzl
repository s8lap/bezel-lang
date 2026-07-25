// §22.8 (v0.226) single-assignment of readonly fields (#2206, route A).
//
// A `ref` argument whose operand resolves to a readonly field is a possible
// write through the alias (the callee may store through it); soundness assumes
// it might. After a direct `:=` of the same field, that is a SECOND assignment
// — the same §22.8 finality hole as the `out` route. The fix feeds `ref`
// operands through checkReadonlyOnceness alongside `out`; the second write is
// rejected.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _f: string?; public constructor() _f := "a"; Mutate(ref _f); end constructor; destructor end destructor; public method Mutate(ref x: string?) x := null; end Mutate; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)
