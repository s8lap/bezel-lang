// RUN: %bezelc -c -o %t.lib.o %S/imported_literal_target_false_reject_5328rv__lib.bzl
// RUN: %not %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck --check-prefix=IMPORTED %s
// RUN: %not %bezelc -c %s 2>&1 | FileCheck --check-prefix=LOCAL %s
//
// Review row for issue #5328 / spec §15.4 (v0.313) — FALSE-REJECT PROBE 4 of
// 5: a USER-DECLARED interface formal, and its SAME-MODULE twin, pinned
// together.
//
// §15.4's closed list of targets that admit a collection literal does not
// admit an abstract interface, whatever declared it: "an abstract interface
// has no concrete backing the literal could be materialized into". So
// `TakesShape([1, 2, 3])` at `TakesShape(s: IShape)` must be REJECTED — this
// is the POSITIVE side of the probe set, and it is the row that shows the
// predicate is not merely a ten-name string match on builtin types.
//
// The reason this row exists: `isCollectionLiteralInterfaceTarget` has two
// branches. A user-declared type reaches the first — `t->declaration->kind ==
// Interface` — and only a BUILTIN collection type (whose `declaration` is
// null) reaches the `importedName` simple-name list. Across a module boundary
// it is worth pinning that a user type still takes the FIRST branch, i.e. that
// the cross-boundary spelling does not degrade a user interface into a
// name-list miss. It does not: `IShape` is rejected on the declaration, and
// the diagnostic names it.
//
// The two RUN lines pin the CROSS-MODULE TWIN INVARIANT the commit body
// claims — the same rule, at the same call-argument location, in the same
// words, from sema, in both legs (§2.6; the property #3141 pins and #3173
// keeps). The producer's `iface_local_rejected` row establishes that the twin
// holds for a BUILTIN collection interface; this row establishes it for a
// user-declared one, which reaches the predicate by a different branch. The
// two diagnostics below are character-identical apart from the type name.
//
// The `IMPORTED` prefix additionally forbids the "cannot target the imported
// parameter type here" wording — pre-fix, the imported leg was left to
// codegen's `lowerCallDispatch` arm and produced THAT string at this same
// location, so these are the lines that make the row a pin rather than a
// control. #7070: that prohibition is a claim about the WHOLE output, so it
// is written on BOTH sides of the positive match — a negative directive
// scans only the region between the surrounding matches, and the copy below
// the match alone let a wrong diagnostic emitted FIRST through. The wording
// is quoted here WITHOUT a directive prefix on purpose: FileCheck scans a
// check file for directive names anywhere on a line, so a prose block that
// quoted one would mint a live directive.
module FRP4;

import FR5328Lib;

public class Program
  public static method Main(): int
    Console.WriteLine(FR5328Lib.FROps.TakesShape([1, 2, 3]));
    return 0;
  end Main;
end Program;

// ---- same-module twin, declared below in the same file ------------------

public interface IShapeLocal
  method Area(): int;
end IShapeLocal;

public class FROpsLocal
  public static method TakesShape(s: IShapeLocal): int => s.Area();
end FROpsLocal;

public class ProgramLocal
  public static method Main(): int
    Console.WriteLine(FROpsLocal.TakesShape([1, 2, 3]));
    return 0;
  end Main;
end ProgramLocal;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape and the rule text.
//
// #7070: the pre-positive copy of the "cannot target the imported parameter
// type here" prohibition. A negative directive scans only the region between
// the surrounding matches, so the copy that follows the positive match
// excluded the codegen wording only from the text AFTER the expected
// diagnostic — a wrong diagnostic emitted FIRST still passed the row.
// IMPORTED-NOT: cannot target the imported parameter type here
// #7069: this row rejects in SEMA in both legs, so codegen never runs on it
// and the codegen-only `internal:` marker could never occur here — the
// front end's own ICE marker is the reachable one. It is printed BEFORE any
// user-facing diagnostic, and a negative directive scans only the region
// between the surrounding matches, so it is written above the positive
// match, where it can fire.
// IMPORTED-NOT: internal compiler error
// IMPORTED: :{{[0-9]+}}:{{[0-9]+}}: a collection literal `[...]` cannot target the interface type `IShape`
// IMPORTED-SAME: §15.4 admits only a concrete collection target
// IMPORTED-SAME: (§15.4)
// IMPORTED-NOT: cannot target the imported parameter type here

// LOCAL-NOT: internal compiler error
// LOCAL: :{{[0-9]+}}:{{[0-9]+}}: a collection literal `[...]` cannot target the interface type `IShapeLocal`
// LOCAL-SAME: §15.4 admits only a concrete collection target
// LOCAL-SAME: (§15.4)
