// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3751, the anti-over-reject boundary. The fix admits the `owned`
// call-site marker at a chain ONLY where the chain TARGET's parameter is
// `owned`; against a plain parameter §22.3's spurious-marker rejection stands
// ("writing it ... on a non-`owned` parameter position is likewise an error").
// Before the fix this was rejected too, but for the wrong reason — no formals
// at all — which is why the rejection was indistinguishable from the WRONG
// rejection of `this(owned x)` against an `owned` slot.

module M;
class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;
class PB(n: Res)
  private _k: Res := n;
  public constructor(owned x: Res, tag: bool)
    this(owned x);
  end constructor;
end PB;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `owned` call-site marker is only valid on an argument to an `owned` parameter; this parameter position does not take ownership — remove `owned` (§22.3
