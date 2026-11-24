//! case: s28_3_7901
//! doc: spec
//! section: 28.3
//! title: a nested lambda may not move the enclosing lambda's owned parameter
//! rule: a captured owned binding — an owned local or `owned` parameter of the enclosing body
//!   — is uniformly in **borrow state**
//! cites: 28.3
//! expect: reject
//! diagnostic: captured owned binding
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
  public static method Run(): int
    var f := lambda(owned p: Res): int => do
      var g := lambda(): int => Sink.Take(move p);
      return g();
    end;
    return f(new Res(1));
  end Run;
  public static method Main(): int
    Console.WriteLine(Run());
    return 0;
  end Main;
end Program;
