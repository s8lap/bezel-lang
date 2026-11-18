//! case: s28_14_7355
//! doc: spec
//! section: 28.14
//! title: a plain function parameter forwarded into a base constructor's
//!   `scoped` parameter is rejected
//! rule: or forwarded into a `scoped` parameter position: the callee cannot
//!   see its argument's capture set
//! cites: 28.3
//! expect: reject
//! diagnostic: call-bounded
//! notes: Pins the #8778 fix: `D(f)` has a plain (call-bounded) `f` and
//!   chains it into `B`'s `scoped f`, which stores it. Before the fix this
//!   was admitted and `Make` returned an object whose stored callback
//!   outlived the lambda's capture (heap-use-after-free). The accepting twin
//!   declares `D`'s parameter `scoped`, as s28_14_7353 does.
module Conf;

public class B
  private scoped _f: () -> int;
  public constructor(scoped f: () -> int) _f := f; end;
  public method Run(): int => _f();
end;

public class D : B
  public constructor(f: () -> int) base(f); end;
end;
public static class Program
  public static method Make(n: int): owned D
    var s := "hi" + n.ToString();
    return new D(lambda(): int => s.Length);
  end;
  public static method Main(): int
    using var d := Make(3);
    Console.Write(d.Run());
    return 0;
  end;
end;
