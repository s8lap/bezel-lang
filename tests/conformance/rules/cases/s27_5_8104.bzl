//! case: s27_5_8104
//! doc: spec
//! section: 27.5
//! title: an async method's loop that writes the captured local, awaiting each pass
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!  vice versa.
//! cites: 27.5
//! expect: run
//! output: 35
//! notes: Guard for #9004's state-machine gate: the fix for synchronous loops must not
//!  change a loop in an async method. k = 2, 4, 7, 11 and each closure reads the current k:
//!  s = 24, return 24 + 11 = 35.
module Conf;
public class C
  public static async method Run(): Task<int>
    var k := 1; var s := 0;
    var i := 0;
    while i < 4 do
      i := i + 1;
      k := k + i;
      var h := lambda() => k;
      s := s + h();
      await Task.Delay(0);
    end;
    return s + k;
  end Run;
  public static method Main(): int
    Console.WriteLine(C.Run().Result);
    return 0;
  end Main;
end C;
