//! case: s28_3_8103
//! doc: spec
//! section: 28.3
//! title: an owning inline array local moved into a call outside any nested closure is
//!  admitted
//! rule: A callback that must genuinely consume a value takes an `owned` parameter and is
//!  invoked with the value transferred from outside
//! cites: 28.3, 15.3
//! expect: run
//! output: 0
//! notes: Accepting twin of s28_3_8004 (#8987): the same consuming statement sits in the
//!  enclosing lambda's own body, outside the nested closure, so the binding is not captured
//!  at that use.
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
      var k := Sink.TakeA(o); var g := lambda(): int => 1;
      var q := g();
      return 0;
    end;
    Console.WriteLine(outer());
    return 0;
  end Main;
end Program;
