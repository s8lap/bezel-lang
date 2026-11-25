//! case: s28_9_9613
//! doc: spec
//! section: 28.9
//! title: a function-typed local passed `ref` to a generic callee stays usable
//! rule: Function-typed fields, parameters, and locals are not subject to `owned`
//!   annotations and carry no disposal obligation
//! cites: 28.9, 22.3, 14.5
//! expect: run
//! stdout: 400
//! issue: 9222
module Conf;

public class Program
  public static method Run<T>(ref x: () -> int, v: T): int
    return x();
  end Run;

  public static method Main()
    var sum := 0;
    for var i in 0 ..< 100 do
      var f: () -> int := lambda() => 2;
      var a := Run(ref f, 5);
      var b := f();
      sum := sum + a + b;
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
