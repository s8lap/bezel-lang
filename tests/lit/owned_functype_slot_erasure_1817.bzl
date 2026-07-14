// Clean-compile ACCEPT for issue #1817 sub-task E2 — §28.1 owned-marker erasure
// at a VALUE substitution of a function-type slot (spec §28.1/§27.1/§22.3,
// v0.196).
//
// §28.1 (v0.196): when a bare type parameter carrying an `owned` marker on a
// function-type slot is substituted at a value or runtime-managed type, the
// marker is ERASED — "the marker does not survive into a substituted function
// type's identity (`(owned T) -> ()` at `T = int` *is* `(int) -> ()`, §27.1)".
// So the substituted function type is markerless and, per §27.1 exact-modifier
// assignability, matches a plain function type.
//
// Both function-type slot positions are covered:
//   * PARAMETER slot: `((owned T) -> ())?` at `T = int`  ->  `((int) -> ())?`
//   * RETURN    slot: `(() -> owned T)?`   at `T = int`  ->  `(() -> int)?`
// and a record value substitution (`T = Pt`) exercises the same erasure at a
// user value type. The generic method's declared return type is what the call
// result is typed from (substituteReturnType); each body returns `null` so the
// test isolates the return-type substitution (the nullable function type makes
// `null` a valid body, exercising no parameter structural matching).
//
// The differential witness is the PARAMETER-slot arm (`pi`, `pp`): without the
// E2 fix its assignment is the §27.1 reject "parameter 1 is `owned` but the
// target's is (none)" — modifier slots are part of a function type's identity.
// The RETURN-slot arm (`mi`, `mp`) additionally exercises the funcReturnOwned
// erasure branch and asserts the substituted form is the canonical markerless
// `(() -> int)?`; §27.1 assignability does not itself compare a function type's
// return-owned marker (that marker governs invocation-result ownership), so this
// arm compiles clean with or without the fix — it guards the erased form and the
// spec's "the substituted signature is the unmarked one" contract at the return
// slot, alongside the differential parameter-slot arm.
//
// Asserted two ways (same shape as the #1797 accept control):
//  1. `%bezelc -c` must EXIT ZERO; output goes to build-tree scratch (`%t.o`).
//  2. The diagnostic stream must contain neither an `error` nor any `§`
//     citation — no residual §27.1 reject and no incidental warning.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;

record Pt(X: int, Y: int);

class R<T>
  public constructor() end constructor;
  // `owned` on the function-type PARAMETER slot, over a bare type parameter T.
  public method Id(): ((owned T) -> ())?
    return null;
  end Id;
  // `owned` on the function-type RETURN slot, over the same bare T.
  public method Make(): (() -> owned T)?
    return null;
  end Make;
end R;

class Program
  public static method Main(): int
    // T = int (a primitive value type) — both markers erased.
    using var ri := new R<int>();
    var pi: ((int) -> ())? := ri.Id();
    var mi: (() -> int)?   := ri.Make();
    // T = Pt (a record value type) — same erasure at a user value type.
    using var rp := new R<Pt>();
    var pp: ((Pt) -> ())? := rp.Id();
    var mp: (() -> Pt)?   := rp.Make();
    return 0;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
