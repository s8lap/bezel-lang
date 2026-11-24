//! case: s28_8_1_7815
//! doc: spec
//! section: 28.8.1
//! title: an owned lambda parameter moved in every arm of a case statement is accepted
//! rule: The transfer of an `owned` argument completes when the call is
//!   **entered**
//! cites: 19.2, 28.3.1
//! expect: run
//! stdout: 4
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
    var f := lambda(owned p: Res, n: int): int => do
      var k := 0;
      case n of
        1 => k := Sink.Take(move p);,
        _ => k := Sink.Take(move p);
      end;
      return k;
    end;
    Console.WriteLine(f(new Res(4), 1));
    return 0;
  end Main;
end Program;
