// RUN: %not %bezelc -c %s 2>&1 | FileCheck --check-prefix=CHECK %s
//
// fix-forward #3575 (§35.4 / §35.3 / §28.8, v0.334): a call through a
// FUNCTION-TYPED FIELD is not a fresh producer. `Box._fetch` is a field, so
// `b._fetch()` has no `MethodSymbol`; its result is a BORROW of the owned
// `Box._h` field, exactly as the method spelling `b.Get()` is. §35.3 gives an
// owning construction — and only an owning construction — the "owned move-only
// value" classification, so passing this one to an `owned` sink is §28.8's
// rejection.
//
// This is the spelling non-author review found MISSING from the first
// attempt's coverage, which special-cased only the method spelling. MEASURED
// on that attempt's arm: this program compiled and then disposed the payload
// twice — ASan `heap-use-after-free … in Box$DisposeCore`, freed by `main`,
// allocated by `Box$ctor`. The arm's "callee symbol is not a MethodSymbol, so
// it must be a fresh producer" inference is what admits it; the corrected arm
// tests POSITIVELY for the case-construction symbol and this row holds.

module M;
class Resource
  private _n: int;
  public constructor(n: int) _n := n; end constructor;
  public method N(): int => _n;
end Resource;
choice Bundle
  Ready(owned r: Resource),
  Empty;
end Bundle;
class Box
  public owned _h: Bundle;
  public _fetch: () -> Bundle;
  public constructor()
    _h := Bundle.Ready(new Resource(42));
    _fetch := lambda() => _h;
  end constructor;
  public method Take(owned h: Bundle) end;
end Box;
class C
  public static method Main(): int
    var b := new Box();
    var h := b._fetch();
    b.Take(move h);
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow cannot be passed
// CHECK-SAME: (§28.8