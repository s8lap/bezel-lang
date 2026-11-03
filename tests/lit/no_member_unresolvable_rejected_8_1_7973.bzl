// fix #7973 (§8.1, §26.5, v0.350): an undeclared member on a root `Object`
// receiver is a COMPILE ERROR, not a codegen degrade.
//
// THIS CASE USED TO PIN THE OPPOSITE. It was
// `no_member_unresolvable_warns_2477.bzl`, and it pinned the #2477 codegen
// diagnostic (`codegen cannot dispatch member 'Anything' on receiver of type
// 'Object': permissive acceptance (spec §3 / issue #605)`) that the
// `Object`-receiver call shape produced — the program's only observable.
//
// §8.1 (v0.350) declares `public class Object` with exactly `GetType`,
// `Equals`, `GetHashCode`, `ToString` and the static `ReferenceEquals`, and
// says of that declaration: "The declaration above defines the public
// surface." §26.5 resolves a member against the receiver's DECLARED type, so
// `o.Anything()` names no member of `Object` and is rejected in sema —
// before codegen ever runs. The "permissive acceptance (spec §3 / issue
// #605)" the old diagnostic carried is a STALE citation (issue #7024:
// "`spec #3` does not name a section of the current spec, and `#605` is closed
// and concerns an unrelated codegen issue"), not a spec rule.
//
// The #2477 DIAGNOSTIC is not retired by this: its sibling arm — the dead
// abstract template body's `new T()` — is still reachable and still pinned, by
// `tests/lit/generic_new_T_dead_template_body_warns_2477.bzl`.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %not %bezelc -c -o %t.o %s 2>&1 | FileCheck %s

module M; class C public method M(o: Object): int var s := o.Anything(); return 0; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Anything` on type `Object`: `Object`'s public surface is exactly `GetType`, `Equals`, `GetHashCode`, `ToString` and the static `ReferenceEquals` (§8.1); members resolve against the declared type (§26.5), and no extension on `Object` provides `Anything`
