//! case: s27_5_7418
//! doc: spec
//! section: 27.5
//! title: a returned closure sees a frame write to the captured local after a sibling forwarder ran
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice versa.
//! cites: 27.5
//! expect: run
//! output: 2800
module Conf; public class C
  public static method Make(): () -> int
    var k := 3; var esc := lambda() => k; var f2 := lambda() => lambda() => k + 1;
    var a := f2()(); k := k + a;
    return esc;
  end Make;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do var g := C.Make(); total := total + g() + g(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
