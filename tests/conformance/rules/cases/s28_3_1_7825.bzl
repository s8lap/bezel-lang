//! case: s28_3_1_7825
//! doc: spec
//! section: 28.3.1
//! title: an owned local moved in the right operand of ?? in a return operand is disposed on the skip path
//! rule: the right operand is evaluated only when the left operand's value is
//!   `null`
//! cites: 7.7, 28.3.1
//! expect: run
//! output: 700
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
  public static method Run(k: int?): int
    var y := new Res(2);
    return k ?? Sink.Take(move y);
  end Run;
  public static method Main(): int
    var s := 0;
    for var i in 0..<100 do
      s := s + Run(5);
      s := s + Run(null);
    end;
    Console.WriteLine(s);
    Console.WriteLine(Res.Live);
    return 0;
  end Main;
end Program;
