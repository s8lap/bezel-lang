//! case: s28_8_1_7808
//! doc: spec
//! section: 28.8.1
//! title: a catch arm reading the moved one of two lambda parameters is rejected
//! rule: a `catch` arm may not dispose the argument a second time — the binding
//!   is unreadable there, as after any move
//! cites: 31.2.2, 28.3.1
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
    var f := lambda(owned p: Res, owned q: Res): int => do
      var k := 0;
      try
        k := Sink.TakeBoom(move p);
      catch
        var e: Exception when q.Tag > 0 => k := p.Tag;
      end;
      return k + Sink.Take(move q);
    end;
    return f(new Res(1), new Res(5));
  end Main;
end Program;
