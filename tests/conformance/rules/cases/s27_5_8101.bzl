//! case: s27_5_8101
//! doc: spec
//! section: 27.5
//! title: closures formed in a loop over a local the loop keeps writing all see its final
//!  value
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!  vice versa.
//! cites: 27.5
//! expect: run
//! output: 4024
//! notes: Pins #9004. One cell per binding: k is 3, 3, 4, 6 across the loop, then 1006
//!  after it; each of the three closures reads 1006 and the method returns 3018 + 1006 =
//!  4024.
module Conf;
public class C
  public static method Run(): int
    var k := 3;
    var fs := new Array<() -> int>();
    for var i in 0..<3 do
      k := k + i;
      fs.Add(lambda() => k);
    end;
    k := k + 1000;
    var s := 0;
    for var f in fs do s := s + f(); end;
    return s + k;
  end Run;
  public static method Main(): int
    Console.WriteLine(C.Run());
    return 0;
  end Main;
end C;
