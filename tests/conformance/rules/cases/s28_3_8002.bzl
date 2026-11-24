//! case: s28_3_8002
//! doc: spec
//! section: 28.3
//! title: a nested lambda passing an enclosing lambda's owning choice local to a call
//!   consumes it
//! rule: assignment to it, a marked transfer from it, an explicit `Dispose()` or discard of
//!   it, and every other owned-consuming use of it are compile errors
//! cites: 28.3, 15.3, 27.5
//! expect: reject
//! diagnostic: captured owned
//! notes: Pins #8987. A move-only argument is a by-value move at either parameter spelling
//!   (15.3), so `Peek(o)` is an owned-consuming use of the captured binding. Twin:
//!   s28_3_8001 (read-only use admitted).
module Conf;

public class Res
  public static Count: int := 0;
  public N: int := 7;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;
public choice OC Text(owned r: Res), Empty; end OC;

public class Sink
  public static method Peek(o: OC): int return 2; end Peek;
end Sink;
public class Program
  public static method Main(): int
    var outer := lambda(): int => do
      var o := OC.Text(new Res());
      var g := lambda(): int => do var k := Sink.Peek(o); return 1; end;
      var q := g();
      return 0;
    end;
    return outer();
  end Main;
end Program;
