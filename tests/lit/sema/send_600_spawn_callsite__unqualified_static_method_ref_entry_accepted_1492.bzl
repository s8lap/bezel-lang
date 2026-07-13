// Ported from tests/sema/send_600_spawn_callsite_test.cpp
//   (BEZEL_TEST send_600_spawn_callsite, unqualified_static_method_ref_entry_accepted_1492).
//
// fix #1492 / §30.14: an UNQUALIFIED static method reference (same class,
// no receiver) as entry with no args → accepted, exactly like the qualified
// `Type.Method` spelling above. In a static class no receiver can ever
// exist, so an unqualified reference is exactly as capture-free as the
// qualified one — the classifier previously recognized only the qualified
// MemberAccessExpr form and wrongly rejected this bare IdentifierExpr form.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Main public static method Run() end Run; public static async method Go(): Task using var t := Domain.Spawn(Run); await t; end Go; end Main;

// NODIAG-NOT: error
// NODIAG-NOT: §
