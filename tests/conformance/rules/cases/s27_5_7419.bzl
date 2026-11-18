//! case: s27_5_7419
//! doc: spec
//! section: 27.5
//! title: two activations of a lambda body each write the method local captured through a forwarder
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice versa.
//! cites: 27.5
//! expect: run
//! output: 3200
module Conf; public class C
  public static method Run(): int
    var k := 3;
    var outer := lambda() => lambda() => do var f2 := lambda() => lambda() => k + 1; k := k + 1; return f2()(); end;
    var a := outer()(); var b := outer()(); return a + b + k;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
