// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Issue #5334 (residual of #5314) — the issue's RELATED SHAPE, as a
// CONTROL row. With the protected block scoped, a local declared there
// SHADOWS a same-named parameter for the length of the block only (§21.1,
// §2.7 rule 1: a bare name resolves through local scope in lexical order).
// After the matching `end try` the name is the PARAMETER again, so the
// member lookup sees the parameter's type.
//
// MEASURED PRE-FIX (staging r23-c28 96b1642c): REJECTED with "no member
// `Length` on primitive type `int`" — sema resolved the post-`try` `v` to
// the leaked `int` local declared in the protected block. That is exactly
// the #5314 defect shape, one block kind over.
//
// Post-fix: accepted; the read after the `end try` sees the `string`
// parameter.
//
// ACCEPT: a protected-block local shadowing a parameter does not change the
// parameter's resolution after the `end try`.

module M; class C public static method F(v: string): int try var v := 5; finally Console.WriteLine("f"); end try; return v.Length; end F; public static method Main(): int Console.WriteLine(C.F("abcd")); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
