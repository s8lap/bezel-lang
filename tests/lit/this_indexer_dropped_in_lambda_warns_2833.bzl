// #2795 follow-up (codegen, LlvmBackend_Assign.cpp `Emitter::indexReceiverDecl`,
// spec §23.4 / §27.3, v0.240): the SILENCE guard for a DROPPED `this[i]`.
//
// §23.4: "Inside a member of a class with its own indexer, `this[i]` is required
// (the brackets need a left-hand side)." #2795 gave that spelling a receiver
// type in sema, which is correct — but codegen recovers the receiver TypeSymbol
// for a `this`-rooted index by a reverse lookup of `currentFn_` in `methodFns_`
// (the #747 R42 branch), and a LAMBDA body's `llvm::Function` is not a value in
// that map. The lookup fails, `lowerIndexerGet` returns null, and the read
// degrades to a zero/null value — measured 0 both before and after #2795, so
// the wrong value is #2833's, not #2795's.
//
// What #2795 DID change is the noise: before it, an untyped `this[n]` left the
// lambda's return type unknown, so the CALLER's `return f(i);` drew an
// incidental `#2491` note. With the type available the caller lowers cleanly and
// the wrong value became FULLY SILENT. This guard restores the announcement at
// the exact point the value is dropped.
//
// ⚠ MAINTENANCE NOTE: this test asserts the GUARD, not the drop. When #2833 is
// fixed — i.e. when codegen can recover the receiver inside a lambda body — the
// CHECK stops matching. That is a CORRECT signal, and the right response is to
// delete this file and replace it with a VALUE test in the #2833 fix (the
// lambda below must then return 91), NOT to weaken the assertion.
//
// The CHECK-NOT half is the load-bearing accept-control: every context where
// the recovery SUCCEEDS must stay silent, so the guard cannot be satisfied by a
// predicate that simply warns on every `this[i]`. Exactly one `#2833` line may
// appear in the whole compile — a second occurrence anywhere (before or after
// the matched one) trips the CHECK-NOT.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;

class Base
  public virtual property [i: int]: int => i + 70;
end Base;

class Acc: Base
  public override property [i: int]: int => i + 90;

  // Accept-control: a PLAIN method body. The `methodFns_` reverse lookup
  // succeeds, the read lowers, and nothing is dropped.
  public method Plain(i: int): int => this[i];

  // Accept-control: a GENERIC method body. The specialization path resolves the
  // receiver through `currentSpecMethodSymbol_`, so it must stay silent too —
  // this is the case an `inLambda_`-style source-category test in sema would
  // have gotten right but the ASYNC body would not, which is why the guard is
  // gated on the recovery having failed rather than on a body kind.
  public method Spec<T>(i: int, t: T): int => this[i];

  // Accept-control: `base[i]`. The recovery SUCCEEDS here (it resolves to the
  // derived type — that is #2832's separate mis-dispatch defect, a wrong value
  // rather than a dropped one), so this guard must NOT claim a drop for it.
  public method ViaBase(i: int): int => base[i];

  // THE PINNED SHAPE: `this[i]` inside a lambda body. Dropped; degrades to 0.
  public method ViaLambda(i: int): int
    var f := lambda(n: int) => this[n];
    return f(i);
  end ViaLambda;
end Acc;

// Accept-control: an EXTENSION body on a class with an indexer. `currentType_`
// is the extended type and the receiver resolves through the extension branch.
class Ext
  public property [i: int]: int => i + 40;
end Ext;

extension Ext
  public method Get(i: int): int => this[i];
end;

class C
  public static method Main(): int
    using var a := new Acc();
    using var e := new Ext();
    var t := a.Plain(1) + a.Spec<int>(1, 0) + a.ViaBase(1) + e.Get(1);
    return t + a.ViaLambda(1);
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: internal: this `this[i]` indexer access was DROPPED — the receiver could not be recovered for this body kind (a lambda body or an `async` body); the read degrades to a zero/null value and a write is silently discarded (§23.4) — #2833
// CHECK-NOT: #2833
