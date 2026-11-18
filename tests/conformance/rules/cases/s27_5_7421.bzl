//! case: s27_5_7421
//! doc: spec
//! section: 27.5
//! title: a forwarder in a loop body sees the outer local and a fresh loop-variable binding per
//!   iteration
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter
//!   excepted (below).
//! cites: 27.5
//! expect: run
//! output: 6200
module Conf; public class C
  public static method Run(): int
    var k := 1; var t := 0;
    for var i in 0..<5 do var f2 := lambda() => lambda() => k + i; t := t + f2()(); k := k + 1; end;
    return t + k;
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
