// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, class_with_nested_serializable_class_field_eligible_spec_9_8).
//
// Regression guard: the record-only restriction above must NOT affect a
// CLASS holding a nested serializable-class component — §9.8's class
// synthesis explicitly admits "another conforming class" as an owning edge
// ("recursively admitting other conforming classes ..."). Only records are
// restricted (§9.8 v0.182).
//
// Reject control: bezelc -c must reject and emit a §28.3 K3.13 redundant-move diagnostic.
//
// RUN: %not %bezelc -c -o %t.o %s
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Inner: ISerializable public property V: int; public constructor(v: int) V := v; end constructor; end Inner; class Outer: ISerializable public owned Obj: Inner; public constructor(owned obj: Inner) Obj := move obj; end constructor; end Outer;


// CHECK: the `move` marker is redundant on a {{.*}} (§28.3 K3.13 marker economy; K3.1)