// fix #7973 (§8.1, §26.5, v0.350): a member access on a root `Object`
// receiver is a COMPILE ERROR, not a permissive acceptance.
//
// THIS CASE USED TO PIN THE OPPOSITE. It was
// `sema_unknown_member__object_typed_receiver_permissive.bzl`, and it pinned
// the permissive acceptance: "§3: a member access on an `Object`-typed
// receiver (a primitive lookup key, not a Named user type) stays permissive —
// the static type carries no known-complete member set", with a NODIAG check
// that the compile emitted nothing at all.
//
// §8.1 (v0.350) declares `public class Object` with exactly `GetType`,
// `Equals`, `GetHashCode`, `ToString` and the static `ReferenceEquals`, and
// says of that declaration: "The declaration above defines the public
// surface." §26.5 resolves a member against the receiver's DECLARED type, so
// `o.Anything()` names no member of `Object`. The "§3 permissive" rule this
// case pinned is not in the spec: `grep -n "resolve dynamically"
// docs/Bezel_Language_Spec.md` → 0 hits, and the "permissive acceptance
// (spec §3 / issue #605)" wording the codegen degradation carried is a stale
// citation (issue #7024).
//
// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, object_typed_receiver_permissive).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %not %bezelc -c -o %t.o %s 2>&1 | FileCheck %s

module M; class C public method M(o: Object): int var s := o.Anything(); return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Anything` on type `Object`: `Object`'s public surface is exactly `GetType`, `Equals`, `GetHashCode`, `ToString` and the static `ReferenceEquals` (§8.1); members resolve against the declared type (§26.5), and no extension on `Object` provides `Anything`
