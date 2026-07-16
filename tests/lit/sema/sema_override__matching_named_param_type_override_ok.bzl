// Ported from tests/sema/override_signature_test.cpp
//   (BEZEL_TEST sema_override, matching_named_param_type_override_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
// The point of this test is that a parameter of a user-declared NAMED type `T`
// matches between a virtual base method and its override.
//
// The body is a procedure (`_ := 1;` / `_ := 2;`, mirroring the sibling
// exact_nullable_param_type_override_ok test) rather than the original
// `: T => t`. That original body returned a plain-PARAMETER borrow from an
// INSTANCE method, which §28.3 (v0.196, #1885) now correctly rejects — the
// caller attributes the non-`owned` instance return to the receiver, not to
// `t`, so a returned borrow of `t` would dangle once the caller's argument is
// disposed (a use-after-free). The override-signature-matching behaviour this
// test exercises is unaffected by the change.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T end T; class Base public virtual method F(t: T) _ := 1; end F; end Base; class D : Base public override method F(t: T) _ := 2; end F; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
