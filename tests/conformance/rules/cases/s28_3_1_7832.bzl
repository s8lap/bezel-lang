//! case: s28_3_1_7832
//! doc: spec
//! section: 28.3.1
//! title: an explicit Dispose of an owned local after a conditional ?? move is rejected
//! rule: unreadable after the join
//! cites: 7.7, 28.3.1
//! expect: reject
//! diagnostic: maybe-consumed
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
    var t := k ?? Sink.Take(move y);
    y.Dispose();
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Run(null));
    return 0;
  end Main;
end Program;
