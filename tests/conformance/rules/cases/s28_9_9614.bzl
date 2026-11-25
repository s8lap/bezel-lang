//! case: s28_9_9614
//! doc: spec
//! section: 28.9
//! title: a function-typed local still returns after a by-ref invocation
//! rule: Function-typed fields, parameters, and locals are not subject to `owned`
//!   annotations and carry no disposal obligation
//! cites: 28.9, 22.3
//! expect: run
//! stdout: 200
//! issue: 9222
module Conf;

public class Program
  public static method Run(ref x: () -> int): int
    return x();
  end Run;

  public static method Make(): () -> int
    var f: () -> int := lambda() => 2;
    var r := Run(ref f);
    if r <> 2 then
      return lambda() => 0;
    end if;
    return f;
  end Make;

  public static method Main()
    var sum := 0;
    for var i in 0 ..< 100 do
      var g := Make();
      var v := g();
      sum := sum + v;
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
