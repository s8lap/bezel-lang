// fix #3641 (§12.5 / §23.4, v0.286): the ACCEPT control paired with
// sema_abs_impl__unimplemented_abstract_class_indexer_diagnoses.bzl — a
// concrete class that DOES override an inherited abstract CLASS indexer
// must still compile clean. Confirms the new §12.5 obligation (and the
// tightened `candWireable` — an abstract candidate is no longer wireable
// even when LOCAL, see TypeResolution_Checks.cpp) does not false-reject a
// genuine override, which is never itself declared `abstract` (§12.1 forbids
// `abstract override`) and so is unaffected by that tightening.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; abstract class Base public abstract property [i: int]: int; end Base; class D : Base public override property [i: int]: int => i; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
