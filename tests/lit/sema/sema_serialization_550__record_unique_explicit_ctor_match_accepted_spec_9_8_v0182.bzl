// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, record_unique_explicit_ctor_match_accepted_spec_9_8_v0182).
//
// §9.8 v0.182: a body record with exactly ONE explicit constructor whose
// parameters bijection-match the auto-properties by name (case-insensitive,
// order-insensitive: ctor param `height` before `width`, properties declared
// `Width` then `Height`) is accepted — no ambiguity, no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Rectangle: ISerializable, IDeserializable public property Width: decimal; public property Height: decimal; public constructor(height: decimal, width: decimal) Width := width; Height := height; end constructor; end Rectangle;

// NODIAG-NOT: error
// NODIAG-NOT: §
