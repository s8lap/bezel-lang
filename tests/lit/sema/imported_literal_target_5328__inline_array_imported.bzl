// RUN: %bezelc -c -o %t.lib.o %S/imported_literal_target_5328__lib.bzl
// RUN: %bezelc -c %s --import %t.lib.o
// RUN: %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Issue #5328 / spec §15.4 (v0.313) — FIX ROW: an INLINE-ARRAY `[3]int`
// formal, imported, with a matching 3-element literal.
//
// §15.4's first bullet on the closed target list is "Inline arrays `[N]T`,
// `[N]T?`, `[N owned]T`, `[N owned]T?` (the literal must have exactly N
// elements)", and the section's own worked example is `ProcessBuffer([1, 2,
// 3]); // type from parameter`. An inline-array formal is a first-class
// §15.4 target, so `Inl([1, 2, 3])` at `Inl(a: [3]int)` MUST compile.
//
// Before the fix: codegen's `lowerCallDispatch` imported-argument arm called
// `materializeCollectionLiteralForArg` which returns null for InlineArray
// formals (it only handles heap collections), causing the "cannot target the
// imported parameter type here" false reject. The sema-side fix (e84fc8ad)
// added the §15.4 interface-target check on the imported channel but did NOT
// fix this codegen gap.
//
// Fix (item 2): add an InlineArray arm to `lowerCallDispatch`'s imported-
// argument branch, mirroring `buildOrderedArgs`'s inline-array path
// (Assign.cpp lines ~14707-14772). An imported formal's TypeRef has
// kind==InlineArray with a non-null arraySizeValue (the folded extent), so
// `lowerTypeRef` produces the llvm::ArrayType and the element-by-element
// materialization proceeds correctly.
//
// This row was previously a %not row pinning the false reject. The test
// comment said "A future codegen lane that admits this correctly will make
// the `%not` fail, and that is the intended signal to revisit this row".
// This is that signal — the row now asserts the correct behavior.
module App5328Inl;

import Lib5328;

public class Program
  public static method Main(): int
    Console.WriteLine(Lib5328.Ops.Inl([1, 2, 3]));
    return 0;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: cannot target
// NODIAG-NOT: §15.4
// NODIAG-NOT: internal:
