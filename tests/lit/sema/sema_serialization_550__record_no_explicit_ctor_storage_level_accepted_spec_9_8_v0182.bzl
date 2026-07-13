// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, record_no_explicit_ctor_storage_level_accepted_spec_9_8_v0182).
//
// §9.8 v0.182: a body record with NO explicit constructor is unaffected by
// the constructor-selection rule (storage-level construction applies) — no
// error, regardless of how many auto-properties it has.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Rectangle: ISerializable, IDeserializable public property Width: decimal; public property Height: decimal; end Rectangle;

// NODIAG-NOT: error
// NODIAG-NOT: §
