// RUN: %not %bezelc -c %s 2>&1 | FileCheck --check-prefix=CHECK %s
//
// fix-forward #3575 (§35.4 / §35.3 / §28.8, v0.334): the same read reached
// through a function-typed LOCAL rather than a field. `g` is a `LocalSymbol`
// whose body returns the owned `Box._h` field, so `g()` carries neither a
// MethodSymbol nor a case-construction symbol — the sharpest instance of the
// first attempt's "no MethodSymbol means a fresh producer" inference, which
// admitted it and then disposed the payload twice at run time.
//
// Its result is a BORROW, so the `owned` sink rejects it under §28.8, as the
// method and field spellings are rejected.

// A `const` local cannot alias `Box._h` (§15.3 move-only value), so the
// borrow source is reached through a `var` reference to the box itself; the
// lambda body reads `src._h` either way.
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
  public constructor() _h := Bundle.Ready(new Resource(42)); end constructor;
  public method Take(owned h: Bundle) end;
end Box;
class C
  public static method Main(): int
    var b := new Box();
    var src := b;
    var g: () -> Bundle := lambda() => src._h;
    var h := g();
    b.Take(move h);
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow cannot be passed
// CHECK-SAME: (§28.8