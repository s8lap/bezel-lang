//! case: s28_3_7903
//! doc: spec
//! section: 28.3
//! title: a nested lambda may read the enclosing lambda's owned parameter (a borrow)
//! rule: a captured owned binding — an owned local or `owned` parameter of the enclosing body
//!   — is uniformly in **borrow state**
//! cites: 28.3
//! expect: run
//! output: 1
//!   0
module Conf;
import System;

public class Res
  public static Live: int := 0;
  public Tag: int;
  public constructor(t: int)
    Tag := t;
    Res.Live := Res.Live + 1;
  end constructor;
  destructor
    Res.Live := Res.Live - 1;
  end destructor;
end Res;

public class Sink
  public static method Take(owned r: Res): int
    using var h := move r;
    return h.Tag;
  end Take;
end Sink;

public class Program
  public static method Main(): int
    var f := lambda(owned p: Res): int => do
      var g := lambda(): int => p.Tag;
      return g();
    end;
    var s := 0;
    for var i in 0..<1 do s := s + f(new Res(1)); end;
    Console.WriteLine(s);
    Console.WriteLine(Res.Live);
    return 0;
  end Main;
end Program;
