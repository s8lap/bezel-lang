//! case: s27_5_8105
//! doc: spec
//! section: 27.5
//! title: an iterator method's loop that writes the captured local, yielding each pass
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!  vice versa.
//! cites: 27.5
//! expect: run
//! output: 24
//! notes: Guard for #9004's state-machine gate in an iterator: k = 2, 4, 7, 11 yielded in
//!  turn; 2 + 4 + 7 + 11 = 24.
module Conf;
public class C
  public static method Gen(): IEnumerable<int>
    var k := 1;
    var i := 0;
    while i < 4 do
      i := i + 1;
      k := k + i;
      var h := lambda() => k;
      yield return h();
    end;
  end Gen;
  public static method Main(): int
    var t := 0;
    for var v in C.Gen() do t := t + v; end;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
