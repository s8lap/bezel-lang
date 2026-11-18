//! case: s28_14_7358
//! doc: spec
//! section: 28.14
//! title: disposing a class-typed `scoped` source while the holder lives is
//!   rejected
//! rule: it is frozen until the bound's scope exits
//! cites: 28.3
//! expect: reject
//! diagnostic: cannot dispose
//! notes: Pins the #8803 fix: the chained `base(scoped s)` stores the borrow
//!   of `bx`; with the holder `k` live, `bx.Dispose()` would leave a stored
//!   borrow dangling.
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
public class E : B
  public constructor(scoped s: Box) base(scoped s); end;
end;
public static class Program
  public static method Main(): int
    var bx := new Box(4);
    using var k := new E(scoped bx);
    bx.Dispose();
    Console.Write(k.Get());
    return 0;
  end;
end;
