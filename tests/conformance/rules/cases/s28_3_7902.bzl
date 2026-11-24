//! case: s28_3_7902
//! doc: spec
//! section: 28.3
//! title: two sibling lambdas each moving their own owned parameter are admitted
//! rule: a captured owned binding — an owned local or `owned` parameter of the enclosing body
//!   — is uniformly in **borrow state**
//! cites: 28.3
//! expect: run
//! output: 3
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
    var f := lambda(owned p: Res): int => Sink.Take(move p);
    var g := lambda(owned p2: Res): int => Sink.Take(move p2);
    var s := 0;
    for var i in 0..<1 do s := s + f(new Res(1)) + g(new Res(2)); end;
    Console.WriteLine(s);
    Console.WriteLine(Res.Live);
    return 0;
  end Main;
end Program;
