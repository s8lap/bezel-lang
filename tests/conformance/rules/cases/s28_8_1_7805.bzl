//! case: s28_8_1_7805
//! doc: spec
//! section: 28.8.1
//! title: a catch guard in a lambda body may read a local that the try did not move
//! rule: The transfer of an `owned` argument completes when the call is
//!   **entered**
//! cites: 31.2.1, 28.3.1
//! expect: run
//! stdout: 7
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
    var f := lambda(): int => do
      using var x := new Res(7);
      var y := new Res(1);
      var k := 0;
      try
        k := Sink.TakeBoom(move y);
      catch
        var e: Exception when x.Tag > 0 => k := x.Tag;
      end;
      return k;
    end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
