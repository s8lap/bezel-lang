//! case: s28_14_7353
//! doc: spec
//! section: 28.14
//! title: a two-level constructor chain forwards a `scoped` function
//!   parameter to each base's `scoped` one
//! rule: or forward it to another `scoped` parameter whose bounding object is
//!   the same object
//! cites: 22.10
//! expect: run
//! stdout: 890
//! notes: Pins the #8803 fix: `E` forwards its `scoped f` to `D`'s `scoped f`
//!   which forwards it to `B`'s. Each forward targets a `scoped` parameter
//!   whose bounding object is the object under construction. The capture is
//!   the string `s` = "hi" + digits of i (2 + digits: 3 for i<10, 4 for
//!   i<100, 5 below 200): 30 + 360 + 500 = 890.
module Conf;

public class B
  private scoped _f: () -> int;
  public constructor(scoped f: () -> int) _f := f; end;
  public method Run(): int => _f();
end;

public class D : B
  public constructor(scoped f: () -> int) base(f); end;
end;
public class E : D
  public constructor(scoped f: () -> int) base(f); end;
end;

public static class Program
  public static method Main(): int
    var i := 0; var tot := 0;
    while i < 200 do
      var s := "hi" + i.ToString();
      using var k := new E(lambda(): int => s.Length);
      tot := tot + k.Run();
      i := i + 1;
    end;
    Console.Write(tot);
    return 0;
  end;
end;
