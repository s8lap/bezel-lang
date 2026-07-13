// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, non_dollar_rename_positional_field_is_ok_spec_2_15_v0139).
//
// §2.15 v0.139: a non-`$` rename is still accepted (no regression).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record R(X: int serialized "myField"): ISerializable, IDeserializable;

// NODIAG-NOT: error
// NODIAG-NOT: §
