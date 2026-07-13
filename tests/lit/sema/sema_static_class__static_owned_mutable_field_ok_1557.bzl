// Ported from tests/sema/static_class_members_test.cpp
//   (BEZEL_TEST sema_static_class, static_owned_mutable_field_ok_1557).
//
// §11.7 / §11.5 (#1557): a mutable `static owned` field (no `readonly`) on a
// static class is accepted too, when its type clears the orthogonal §22.12
// rule 2 check (a mutable static field of class/interface type must be one
// of the compiler-known internally-synchronized types — Mutex, Semaphore,
// Atomic<T>, etc.). `Mutex` qualifies, so this isolates the #1557 rejection
// (now lifted) from that separate, unrelated rule: a user class like `Held`
// would still be rejected here by rule 2 alone, exactly as it would be on an
// ordinary (non-static) class — the lifted §11.7 ban does not carve out a
// narrower rule for static classes specifically.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class Globals public static owned Lock: Mutex? := new Mutex(); end Globals;

// NODIAG-NOT: error
// NODIAG-NOT: §
