//! case: s28_8_1_7814
//! doc: spec
//! section: 28.8.1
//! title: an owned parameter moved in the try body of a try/finally with no catch is accepted
//! rule: The transfer of an `owned` argument completes when the call is
//!   **entered**
//! cites: 31.2, 28.3.1
//! expect: run
//! stdout: 5
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
        k := Sink.Take(move p);
      finally
        k := k + 1;
      end;
      return k;
    end;
    Console.WriteLine(f(new Res(4)));
    return 0;
  end Main;
end Program;
