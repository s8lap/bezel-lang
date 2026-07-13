// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, object_typed_receiver_permissive).
//
// §3: a member access on an `Object`-typed receiver (a primitive lookup key,
// not a Named user type) stays permissive — the static type carries no
// known-complete member set.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(o: Object): int var s := o.Anything(); return 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
