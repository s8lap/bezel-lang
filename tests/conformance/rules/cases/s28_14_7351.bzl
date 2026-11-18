//! case: s28_14_7351
//! doc: spec
//! section: 28.14
//! title: a chained constructor passes a lambda capturing a string parameter
//!   to a base `scoped` formal
//! rule: The callee may store the callback
//! cites: 22.10
//! expect: run
//! stdout: 690
//! notes: Pins the #8778 fix on the admitting side for a string capture (a
//!   runtime-managed value, carrying no borrow source). `D(n)` builds the
//!   string `s` + n and chains it through `this(...)`; the lambda captures
//!   `s` and is stored by the base. The string is "s" followed by the decimal
//!   digits of i: 1 char for i < 10 (2 total), 3 for i in 10..99, 4 for i in
//!   100..199, so the length sum is 10*2 + 90*3 + 100*4 = 690.
module Conf;

public class B
  private scoped _f: () -> int;
  public constructor(scoped f: () -> int) _f := f; end;
  public method Run(): int => _f();
end;

public class D : B
  public constructor(n: int) this("s" + n.ToString()); end;
  public constructor(s: string) base(lambda(): int => s.Length); end;
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
