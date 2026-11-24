//! case: s28_3_10_7902
//! doc: spec
//! section: 28.3.10
//! title: a lambda ending in a case whose every arm discharges its owned local and returns is
//!   admitted
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10
//! expect: run
//! output: 600
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
    var f := lambda(n: int): int => do
      var x := new Res(n);
      case n of
        0 => do return Sink.Take(move x); end,
        1 => do var t := x.Tag; x.Dispose(); return t; end,
        _ => do x.Dispose(); return 5; end
      end case;
    end;
    var s := 0;
    for var i in 0..<300 do s := s + f(i mod 3); end;
    Console.WriteLine(s);
    Console.WriteLine(Res.Live);
    return 0;
  end Main;
end Program;
