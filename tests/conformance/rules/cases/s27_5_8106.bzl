//! case: s27_5_8106
//! doc: spec
//! section: 27.5
//! title: an async loop that declares a fresh captured local per pass keeps each pass's
//!  value
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!  vice versa.
//! cites: 27.5
//! expect: run
//! output: 100
//! notes: Guard for #9004: a local declared inside the loop body is a fresh binding per
//!  pass, so the four closures read 10, 20, 30 and 40 after the await: 100.
module Conf;
public class C
  public static async method Run(): Task<int>
    var s := 0;
    var i := 0;
    while i < 4 do
      i := i + 1;
      var k := i * 10;
      var h := lambda() => k;
      await Task.Delay(0);
      s := s + h();
    end;
    return s;
  end Run;
  public static method Main(): int
    Console.WriteLine(C.Run().Result);
    return 0;
  end Main;
end C;
