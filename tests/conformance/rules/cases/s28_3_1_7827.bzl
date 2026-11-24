//! case: s28_3_1_7827
//! doc: spec
//! section: 28.3.1
//! title: an early return before a conditional move disposes the owned local on the early path
//! rule: the holder carries the disposal obligation: dispose exactly once, on
//!   every path
//! cites: 7.7, 28.3.1
//! expect: run
//! output: 1600
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
  public static method Run(c: bool, k: int?): int
    var y := new Res(2);
    if c then
      return 9;
    end;
    var t := k ?? Sink.Take(move y);
    return t;
  end Run;
  public static method Main(): int
    var s := 0;
    for var i in 0..<100 do
      s := s + Run(true, null);
      s := s + Run(false, 5);
      s := s + Run(false, null);
    end;
    Console.WriteLine(s);
    Console.WriteLine(Res.Live);
    return 0;
  end Main;
end Program;
