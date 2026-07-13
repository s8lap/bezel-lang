// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, serialized_skip_removes_ineligible_component_spec_9_8).
//
// §9.8: "`serialized skip` interacts with the eligibility rule by *removing*
// its component from consideration: a field that would otherwise make the
// type ineligible … but is marked `serialized skip` does not disqualify the
// type."
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C end C; record R(X: int, Obj: C serialized skip): ISerializable;

// NODIAG-NOT: error
// NODIAG-NOT: §
