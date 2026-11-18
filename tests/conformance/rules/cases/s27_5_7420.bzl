//! case: s27_5_7420
//! doc: spec
//! section: 27.5
//! title: each activation of a lambda body has its own local captured by its forwarder
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter
//!   excepted (below).
//! cites: 27.5
//! expect: run
//! output: 3000
module Conf; public class C
  public static method Run(): int
    var outer := lambda() => lambda() => do var k := 3; var f2 := lambda() => lambda() => k + 1; k := k + 1; return f2()(); end;
    var r1 := outer(); var r2 := outer();
    return r1() + r2() + r1();
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
