//! case: s28_3_8004
//! doc: spec
//! section: 28.3
//! title: a nested lambda handing an enclosing lambda's owning inline array local to a call
//!   consumes it
//! rule: assignment to it, a marked transfer from it, an explicit `Dispose()` or discard of
//!   it, and every other owned-consuming use of it are compile errors
//! cites: 28.3, 15.3, 28.9
//! expect: reject
//! diagnostic: captured owned
//! notes: Pins #8987 for the owning inline array family: the array is move-only (15.3) and
//!   `TakeA(o)` moves it from inside the closure.
module Conf;
public class Res
  public N: int := 7;
  public constructor() end constructor;
end Res;
public class Sink
  public static method TakeA(a: [1 owned]Res?): int return 1; end TakeA;
end Sink;
public class Program
  public static method Main(): int
    var outer := lambda(): int => do
      var o: [1 owned]Res? := [new Res()];
      var g := lambda(): int => do var k := Sink.TakeA(o); return 1; end;
      var q := g();
      return 0;
    end;
    return outer();
  end Main;
end Program;
