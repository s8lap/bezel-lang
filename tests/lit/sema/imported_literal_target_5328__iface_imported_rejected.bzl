// RUN: %bezelc -c -o %t.lib.o %S/imported_literal_target_5328__lib.bzl
// RUN: %not %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck %s
//
// Issue #5328 / spec §15.4 (v0.313) — PIN ROW 1 (the gap itself): a
// collection literal at an IMPORTED callee's formal of interface type.
//
// §15.4: "A collection literal has no standalone type. It is well-typed only
// in a context that provides the target type: a type annotation, or the
// parameter type of a method call", and the targets that admit one are a
// CLOSED list that does not include a collection INTERFACE. The same-module
// twin of this program has been rejected from sema since #1174 via
// `isCollectionLiteralInterfaceTarget` in `checkArgsAgainstParams`.
//
// The imported path did not run that predicate at all. Pre-fix, the
// character-identical program crossed the boundary and was reported by a
// DIFFERENT check in a DIFFERENT phase with DIFFERENT words — codegen's
// `lowerCallDispatch` imported-argument arm's "cannot target the imported
// parameter type here" — for the same source location. This row pins the
// post-fix behaviour: the SAME rule, from sema, in the SAME words as the
// local leg.
//
// The `CHECK` block is therefore the same-module diagnostic verbatim. That is
// the load-bearing assertion: a downstream `grep -F` and a local row's
// `CHECK` must find one needle in both legs, so a cross-module rejection is
// indistinguishable from the local one at the same location (§2.6; the
// property #3141 pins and #3173 keeps).
//
// The escape guard below — a gtest-style harness maps a compiler CRASH to the
// same signal as a clean reject, so pinning the diagnostic's CONTENT is what
// distinguishes a real §15.4 rejection from an ICE escape. (#6620: that guard
// used to name the codegen-only `internal:` marker — codegen is gated on a
// clean front end, so on a row that rejects in the front end it named a
// needle that could never fire; it now names the front end's own ICE marker.)
module App5328Iface;

import Lib5328;

public class Program
  public static method Main(): int
    Console.WriteLine(Lib5328.Ops.SumE([1, 2, 3]));
    return 0;
  end Main;
end Program;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape and the rule text.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a collection literal `[...]` cannot target the interface type `IEnumerable`
// CHECK-SAME: §15.4 admits only a concrete collection target
// CHECK-SAME: (§15.4)
// CHECK-NOT: cannot target the imported parameter type here
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error
