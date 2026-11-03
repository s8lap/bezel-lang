// fix #7973 (§8.1, §26.5, v0.350): an undeclared member READ on a root
// `Object` receiver is a COMPILE ERROR, not a codegen degrade.
//
// THIS CASE USED TO PIN THE OPPOSITE. It was
// `no_member_unresolvable_field_read_warns_2547.bzl`, and it pinned the #2547
// codegen diagnostic (`codegen cannot read member 'Anything' on receiver of
// type 'Object': permissive acceptance (spec §3 / issue #605)`) that the
// bare-MemberAccessExpr-read shape produced — the program's only observable.
//
// §8.1 (v0.350) declares `public class Object` with exactly `GetType`,
// `Equals`, `GetHashCode`, `ToString` and the static `ReferenceEquals`, and
// says of that declaration: "The declaration above defines the public
// surface." §26.5 resolves a member against the receiver's DECLARED type, so
// the read `o.Anything` names no member of `Object` and is rejected in sema.
//
// The paren-less read and the call form were the same shape with the same
// verdict; the call form's case is
// `tests/lit/no_member_unresolvable_rejected_8_1_7973.bzl`.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %not %bezelc -c -o %t.o %s 2>&1 | FileCheck %s

module M; class C public static method Main(): int var o: Object := new Object(); var s := o.Anything; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Anything` on type `Object`: `Object`'s public surface is exactly `GetType`, `Equals`, `GetHashCode`, `ToString` and the static `ReferenceEquals` (§8.1); members resolve against the declared type (§26.5), and no extension on `Object` provides `Anything`
