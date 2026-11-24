//! case: s28_8_1_7817
//! doc: spec
//! section: 28.8.1
//! title: an owned lambda parameter read in the catch arm and moved only after the try is accepted
//! rule: the obligation has transferred out; the binding is unreadable until
//!   reassigned
//! cites: 31.2.2, 28.3.1
//! expect: run
//! stdout: 6
module Conf;
import System;

public class Res
  public Tag: int;
  public constructor(t: int)
    Tag := t;
  end constructor;
  destructor
  end destructor;
end Res;

public class Sink
  public static method TakeBoom(owned r: Res): int
    throw new InvalidOperationException("boom");
  end TakeBoom;
  public static method Boom(): int
    throw new InvalidOperationException("boom");
  end Boom;
  public static method Take(owned r: Res): int
    using var h := move r;
    return h.Tag;
  end Take;
  public static method Peek(r: Res): int
    return r.Tag;
  end Peek;
end Sink;

public class Program
  public static method Main(): int
    var f := lambda(owned p: Res): int => do
      var k := 0;
      try
        k := Sink.Boom();
      catch
        var e: Exception => k := p.Tag;
      end;
      return k + Sink.Take(move p);
    end;
    Console.WriteLine(f(new Res(3)));
    return 0;
  end Main;
end Program;
