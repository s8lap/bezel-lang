// RUN: %bezelc -c -o %t.lib.o %S/imported_literal_target_5328__lib.bzl
// RUN: %bezelc -c %s --import %t.lib.o
// RUN: %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Issue #5328 / spec §15.4 (v0.313) — CONTROL ROW: a `Set<int>` formal,
// imported, with a DUPLICATE literal element.
//
// §15.4: "`Set<T>`, `OwningSet<T>` (sets)" is ON the closed target list, and
// "For `Set<T>`, `OwningSet<T>`: duplicates are collapsed". So a duplicate in
// the literal is not a typing error at the call site — §15.4's rule is that
// the resulting SET has the collapsed contents, and collapsing is the
// construction's job, not a rejection.
//
// This row exists to pin that the sema side of this lane did NOT over-reject
// while closing the interface gap. It is the row that caught the wider
// variant of the fix: extending the local arm's full #3031
// `isCollectionLiteralAdmissibleTarget` gate across the boundary rejects this
// program outright, with the generic out-of-list diagnostic, because that
// predicate identifies a builtin collection head through `t->astNode` and an
// imported formal's TypeRef carries its name on `importedName` with a null
// `astNode` (issue #5328 item 4's `importedName`-vs-`astNode` blind spot).
// MEASURED, pre-narrowing: `cannot target the type `Set<int>`` — a false
// reject of a target §15.4 admits.
//
// The issue's own item 3 (a duplicate `Set` element not being collapsed, so
// the callee saw 3 elements where the same-module twin saw 2) is a CODEGEN
// defect and is NOT fixed by this lane: sema admits the call, and what the
// imported argument materializer then does with the duplicate is the separate
// codegen arm. Two RUN lines assert the accept two ways, per the
// xmod_static_interface_accept_2294 pattern — the compile must EXIT ZERO, and
// the diagnostic stream must contain no error.
module App5328Set;

import Lib5328;

public class Program
  public static method Main(): int
    Console.WriteLine(Lib5328.Ops.St([1, 2, 2]));
    return 0;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: cannot target
// NODIAG-NOT: §15.4
// NODIAG-NOT: internal:
