//! case: s7_5_7829
//! doc: spec
//! section: 7.5
//! title: an expression-statement ?. call moving an owned local disposes it when the receiver is null
//! rule: the argument and index expressions of the skipped suffixes are not
//!   evaluated either
//! cites: 28.3.1, 7.5
//! expect: run
//! output: 200
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

public class Box
  public method Take(owned r: Res): int
    using var h := move r;
    return h.Tag;
  end Take;
end Box;

public class Program
  public static method Run(h: Box?): int
    var y := new Res(2);
    h?.Take(move y);
    return 1;
  end Run;
  public static method Main(): int
    using var b := new Box();
    var s := 0;
    for var i in 0..<100 do
      s := s + Run(b);
      s := s + Run(null);
    end;
    Console.WriteLine(s);
    Console.WriteLine(Res.Live);
    return 0;
  end Main;
end Program;
