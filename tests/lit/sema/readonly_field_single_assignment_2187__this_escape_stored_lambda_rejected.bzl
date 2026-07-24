// §22.8 / §22.7 / §7.4 (v0.226) — the #2187 probe-confirmed SEGV, now rejected.
//
// The exploit: the ctor assigns `_f := "abc"`, a this-escape helper (§22.7)
// observes `_f <> null` and STASHES a lambda whose body reads `_f.Length`
// (the ambient non-null fact rides in — §7.4, sound ONLY because a readonly
// field's assigned value is final), then the ctor re-assigns `_f := null`.
// Invoking the stashed lambda later derefs null → `SUMMARY: AddressSanitizer:
// SEGV ... in lam$0` on main. The ill-formedness is the DOUBLE ASSIGNMENT
// (§22.8); rejecting it closes the hole.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
class C
  public readonly _f: string?;
  public _stash: (() -> int)?;
  public constructor() _f := "abc"; Helper(); _f := null; end constructor;
  destructor end destructor;
  public method Helper()
    if _f <> null then _stash := lambda(): int => _f.Length; end;
  end Helper;
end C;
class Prog
  public static method Main(): int
    using var c := new C(); var s := c._stash;
    if s <> null then return s(); end; return -1;
  end Main;
end Prog;

// CHECK: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)
