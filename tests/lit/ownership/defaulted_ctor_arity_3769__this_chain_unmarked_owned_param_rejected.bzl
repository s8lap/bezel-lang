// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3769, half (b): the `this(...)` chain twin. §12.8 puts the chain call
// in the leading statement position; with the §22.5 default omitted the chain
// matched neither the §22.10 primary (arity 2) nor the enclosing convenience
// constructor (§22.10 forbids the self-chain), so its argument was classified
// against nothing. It compiled with ONLY the §28.3 auto-drop warning and was
// `AddressSanitizer: heap-use-after-free` at run time.

module M;

class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;

class Base(owned r: Res, tag: int := 5)
  private owned _s: Res := r;
  private _t: int := tag;
  public constructor(owned x: Res, flag: bool)
    // CHECK: this call consumes `x`
    // CHECK-SAME: §22.3
    this(x);
  end constructor;
  public method Id(): int
    return _t + _s.Id();
  end Id;
end Base;
