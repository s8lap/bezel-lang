//! case: s28_14_7354
//! doc: spec
//! section: 28.14
//! title: a two-level constructor chain forwards a class-typed `scoped`
//!   parameter with the call-site marker
//! rule: may forward it only to another `scoped` parameter with the same
//!   bounding object
//! cites: 22.3
//! expect: run
//! stdout: 20100
//! notes: Pins the #8803 fix for the class-typed `scoped` form: `E(scoped s)`
//!   forwards `scoped s` to `D`, which forwards it to `B`, which stores it in
//!   a non-owned field. `bx` is declared before `k`, so it outlives it. Sum
//!   of (i + 1) over 200 iterations is 20100.
module Conf;

public class Box
  public v: int;
  public constructor(x: int) v := x; end;
end;

public class B
  private src: Box;
  public constructor(scoped s: Box) src := s; end;
  public method Get(): int => src.v;
end;
public class D : B
  public constructor(scoped s: Box) base(scoped s); end;
end;
public class E : D
  public constructor(scoped s: Box) base(scoped s); end;
end;

public static class Program
  public static method Main(): int
    var i := 0; var tot := 0;
    while i < 200 do
      using var bx := new Box(i);
      using var k := new E(scoped bx);
      tot := tot + k.Get() + 1;
      i := i + 1;
    end;
    Console.Write(tot);
    return 0;
  end;
end;
