//! case: s28_3_1_7822
//! doc: spec
//! section: 28.3.1
//! title: an owned local moved in the right operand of or is disposed on the short-circuit path
//! rule: `and` and `or` short-circuit: the right operand is not evaluated if the
//!   result is determined by the left.
//! cites: 17.2, 28.3.1
//! expect: run
//! output: 100
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
  public static method Run(c: bool): int
    var x := new Res(0);
    var b := c or Sink.Take(move x) > 0;
    if b then
      return 1;
    end;
    return 0;
  end Run;
  public static method Main(): int
    var s := 0;
    for var i in 0..<100 do
      s := s + Run(true);
      s := s + Run(false);
    end;
    Console.WriteLine(s);
    Console.WriteLine(Res.Live);
    return 0;
  end Main;
end Program;
