// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, class_with_nested_serializable_class_field_eligible_spec_9_8).
//
// Regression guard: the record-only restriction above must NOT affect a
// CLASS holding a nested serializable-class component — §9.8's class
// synthesis explicitly admits "another conforming class" as an owning edge
// ("recursively admitting other conforming classes ..."). Only records are
// restricted (§9.8 v0.182).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Inner: ISerializable public property V: int; public constructor(v: int) V := v; end constructor; end Inner; class Outer: ISerializable public owned Obj: Inner; public constructor(owned obj: Inner) Obj := obj; end constructor; end Outer;

// NODIAG-NOT: error
// NODIAG-NOT: §
