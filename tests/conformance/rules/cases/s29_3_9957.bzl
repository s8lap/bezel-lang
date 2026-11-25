//! case: s29_3_9957
//! doc: spec
//! section: 29.3
//! title: a yield operand invoking a lambda over a string local leaves the string readable afterwards
//! rule: `yield return expression;` produces the next value and suspends.
//! cites: 29.3, 27.5
//! expect: run
//! output: 21780
//! notes: Pins #9166 (managed-string capture). name = "ab" + k.ToString() has length 3 for k 0..9 (10
//!   values), 4 for 10..99 (90), 5 for 100..199 (100): total length 30 + 360 + 500 = 890. Each pass
//!   yields len and len + 100: 2 * 890 + 100 * 200 = 1780 + 20000 = 21780.
module Conf;
import System;
public class It
  public static method Each(n: int): IEnumerable<int>
    var k := 0;
    while k < n do
      var name := "ab" + k.ToString();
      yield return (lambda() => name.Length)();
      yield return name.Length + 100;
      k := k + 1;
    end while;
  end Each;
end It;
public class Program
  public static method Main(): int
    var s := 0;
    for var x in It.Each(200) do s := s + x; end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
