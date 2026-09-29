// Ported from tests/sema/static_field_domain_reachable_4675_test.cpp
//   (BEZEL_TEST static_field_domain_reachable, class_type_rule4 and value_type_rule4).
//
// REGRESSION PIN (§22.12 rule 4, stdlib §6.9/§6.11, v0.307 / #4675):
// Two emission sites in checkTypeStaticFieldDomains were missing the literal
// word "warning" in their Severity::Warning diagnostic strings — the (3a)
// class/interface path and the (3d) value-type residual path.  Severity::Warning
// already emits the "warning:" prefix, so the fix inserts "a warning:" at the
// point of obligation in the free-text message, matching the convention used
// elsewhere in the same TU.
//
// This is an ACCEPT-WITH-WARNING test: bezelc exits zero (the program is
// legal) but emits the pinned diagnostic.  The FileCheck pattern matches the
// FULL warning line — including the literal word "warning" in the body — so
// it fails on the pre-fix compiler (which emits the diagnostic WITHOUT the
// embedded "warning" word) and passes on the post-fix compiler.
//
// Case (a): static readonly field of class type → rule 4 class/interface path.
// Case (d): static readonly field of value type  → rule 4 residual path.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;

// Case (a): static readonly field of class type — triggers the class/interface
// rule 4 path.  Box is a user-declared class, not in the synchronized or
// readonly-suppression sets, so it hits isRule4GovernedReference and emits the
// class-type diagnostic with "a warning: confine".
class Box public constructor() end constructor; end Box;
class C1 private static readonly box: Box := new Box(); end C1;

// Case (d): static readonly field of record type — triggers the residual rule 4
// path.  Pair is a record (not a user class), not in synchronized/readonly-
// suppression sets, and not rule-3 immutable content (not a primitive/string/enum),
// so it falls to the residual arm and emits the value-type diagnostic with
// "a warning: confine".
record Pair(X: int, Y: int);
class C2 private static readonly pair: Pair; end C2;

// CHECK: warning: static readonly field {{.*}} has class type {{.*}} which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchronization (§22.12 rule 4, #1363)

// CHECK: warning: static readonly field {{.*}} has record type {{.*}} which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchronization (§22.12 rule 4 residual, v0.265, #3273 / #3290)
