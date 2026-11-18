//! case: s28_14_7350
//! doc: spec
//! section: 28.14
//! title: a derived constructor chains a value-capturing lambda into a base
//!   `scoped` function parameter
//! rule: The callee may store the callback
//! cites: 22.10
//! expect: run
//! stdout: 20100
//! notes: Pins the #8778 fix on the admitting side: `D(n)` passes `lambda():
//!   int => n + 1` to the base constructor's `scoped f`. The lambda captures
//!   `n`, an `int`, so it carries no borrow source and the chain call is
//!   admitted. 200 iterations sum 1 + 2 + ... + 200 = 20100.
module Conf;

public class B
  private scoped _f: () -> int;
  public constructor(scoped f: () -> int) _f := f; end;
  public method Run(): int => _f();
end;

public class D : B
  public constructor(n: int) base(lambda(): int => n + 1); end;
end;

public static class Program
  public static method Main(): int
    var i := 0; var tot := 0;
    while i < 200 do
      using var k := new D(i);
      tot := tot + k.Run();
      i := i + 1;
    end;
    Console.Write(tot);
    return 0;
  end;
end;
