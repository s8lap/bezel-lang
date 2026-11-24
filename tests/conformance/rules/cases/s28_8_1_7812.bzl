//! case: s28_8_1_7812
//! doc: spec
//! section: 28.8.1
//! title: two catch arms may each guard-read an owned parameter that the try did not move
//! rule: The transfer of an `owned` argument completes when the call is
//!   **entered**
//! cites: 31.2.1, 31.2.3, 28.3.1
//! expect: run
//! stdout: 8
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
        k := Sink.TakeBoom(new Res(1));
      catch
        var e: InvalidOperationException when p.Tag > 100 => k := 1;,
        var e2: Exception when p.Tag > 0 => k := p.Tag;
      end;
      return k + Sink.Take(move p);
    end;
    Console.WriteLine(f(new Res(4)));
    return 0;
  end Main;
end Program;
