//! case: s27_5_8102
//! doc: spec
//! section: 27.5
//! title: a loop that writes the captured local before and after the closure, with continue
//!  and break
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!  vice versa.
//! cites: 27.5
//! expect: run
//! output: 239
//! notes: Pins #9004. Trace: i=1 k=2, h=2, s=2, k=4; i=2 k=6; i=3 k=9, s=11, k=18; i=4
//!  k=22; i=5 k=27, s=38, k=54; i=6 k=60; i=7 k=67, s=105, k=134, break; 105 + 134 = 239.
module Conf;
public class C
  public static method Run(): int
    var k := 1; var s := 0;
    var i := 0;
    while i < 10 do
      i := i + 1;
      k := k + i;
      if i mod 2 = 0 then continue; end;
      var h := lambda() => k;
      s := s + h();
      k := k * 2;
      if i = 7 then break; end;
    end;
    return s + k;
  end Run;
  public static method Main(): int
    Console.WriteLine(C.Run());
    return 0;
  end Main;
end C;
