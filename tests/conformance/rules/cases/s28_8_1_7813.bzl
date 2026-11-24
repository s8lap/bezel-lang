//! case: s28_8_1_7813
//! doc: spec
//! section: 28.8.1
//! title: a catch guard may read a non-owned (borrowed) lambda parameter and pass it to a borrowing call
//! rule: no obligation; validity is bounded by the borrow's source set
//! cites: 31.2.1, 28.3.1
//! expect: run
//! stdout: 3
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
    using var r := new Res(3);
    var f := lambda(p: Res): int => do
      var k := 0;
      try
        k := Sink.TakeBoom(new Res(1));
      catch
        var e: Exception when p.Tag > 0 => k := Sink.Peek(p);
      end;
      return k;
    end;
    Console.WriteLine(f(r));
    return 0;
  end Main;
end Program;
