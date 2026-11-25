//! case: s28_9_9612
//! doc: spec
//! section: 28.9
//! title: a method-reference function-typed local passed `ref` to an invoking callee stays usable
//! rule: Function-typed fields, parameters, and locals are not subject to `owned`
//!   annotations and carry no disposal obligation
//! cites: 28.9, 22.3, 27.6
//! expect: run
//! stdout: 400
//! issue: 9222
module Conf;

public class Program
  public static method Two(): int => 2;

  public static method Run(ref x: () -> int): int
    return x();
  end Run;

  public static method Main()
    var sum := 0;
    for var i in 0 ..< 100 do
      var f: () -> int := Two;
      var a := Run(ref f);
      var b := f();
      sum := sum + a + b;
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
