//! case: s28_3_10_7901
//! doc: spec
//! section: 28.3.10
//! title: a lambda whose if/elif/else arms all return, each discharging its owned local, is
//!   admitted
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10
//! expect: run
//! output: 550
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
    var f := lambda(fl: bool, k: int): int => do
      var x := new Res(1);
      if fl then return Sink.Take(move x);
      elif k > 1 then x.Dispose(); return 4;
      else x.Dispose(); return 5;
      end;
    end;
    var s := 0;
    for var i in 0..<200 do s := s + f(i mod 2 = 0, i mod 4); end;
    Console.WriteLine(s);
    Console.WriteLine(Res.Live);
    return 0;
  end Main;
end Program;
