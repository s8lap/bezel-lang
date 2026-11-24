//! case: s28_3_8101
//! doc: spec
//! section: 28.3
//! title: an owning choice local passed to a call outside any nested closure is admitted
//! rule: A callback that must genuinely consume a value takes an `owned` parameter and is
//!  invoked with the value transferred from outside
//! cites: 28.3, 15.3
//! expect: run
//! output: 0
//! notes: Accepting twin of s28_3_8002 (#8987): the same consuming statement sits in the
//!  enclosing lambda's own body, outside the nested closure, so the binding is not captured
//!  at that use.
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
      var k := Sink.Peek(o); var g := lambda(): int => 1;
      var q := g();
      return 0;
    end;
    Console.WriteLine(outer());
    return 0;
  end Main;
end Program;
