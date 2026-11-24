//! case: s28_3_1_7831
//! doc: spec
//! section: 28.3.1
//! title: a second ?? move of the same owned local after a conditional move is rejected
//! rule: owned ⊓ moved-from and owned ⊓ explicitly-disposed = **maybe-consumed**
//!   — unreadable after the join, no flow around a back edge into a use;
//!   scope-end disposal per-path
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
    var u := k ?? Sink.Take(move y);
    return t + u;
  end Run;
  public static method Main(): int
    Console.WriteLine(Run(null));
    return 0;
  end Main;
end Program;
