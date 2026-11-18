//! case: s28_14_7359
//! doc: spec
//! section: 28.14
//! title: a fresh producer passed with `scoped` through a constructor chain
//!   is rejected
//! rule: A fresh producer argument is rejected
//! cites: 9.7
//! expect: reject
//! diagnostic: fresh producer
//! notes: Pins the #8803 fix: `new E(scoped new Box(3))` hands the chain a
//!   temporary that dies at the end of the full expression.
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
    using var k := new E(scoped new Box(3));
    Console.Write(k.Get());
    return 0;
  end;
end;
