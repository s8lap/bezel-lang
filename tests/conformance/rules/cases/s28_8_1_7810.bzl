//! case: s28_8_1_7810
//! doc: spec
//! section: 28.8.1
//! title: a catch guard passing a moved lambda parameter into a nested lambda call is rejected
//! rule: the caller's binding is moved-from on the exception edge exactly as on
//!   the normal one
//! cites: 31.2.1, 28.3.1
//! expect: reject
//! diagnostic: moved
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
        k := Sink.TakeBoom(move p);
      catch
        var e: Exception when (lambda(q: Res): bool => q.Tag > 0)(p) => k := 1;
      end;
      return k;
    end;
    return f(new Res(1));
  end Main;
end Program;
