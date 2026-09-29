// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, record_explicit_ctor_ambiguous_rejected_spec_9_8_v0182).
//
// §9.8 v0.182: a body record with TWO explicit constructors that both
// bijection-match the same component set (by name, ignoring parameter order)
// is ambiguous — a compile error naming both candidates ("more than one …
// matches … ambiguous").

module M; record Ambig: ISerializable, IDeserializable public property N: int; public property M: int; public constructor(n: int, m: int) N := n; M := m; end constructor; public constructor(m: int, n: int) N := n * 2; M := m * 2; end constructor; end Ambig;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Ambig` declares `IDeserializable`: more than one explicit constructor matches the serialized component set — ambiguous: constructor(n, m), constructor(m, n)
// CHECK-SAME: (§9.8 step 2, v0.309: "more than one matches" is read over the explicit constructors, with the synthesized primary consulted only when none matches)
