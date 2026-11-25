//! case: s29_3_9952
//! doc: spec
//! section: 29.3
//! title: a second yield operand invoking a lambda after a plain first yield yields every value
//! rule: `yield return expression;` produces the next value and suspends.
//! cites: 29.3, 27.5
//! expect: run
//! output: 79900
//! notes: Pins #9166. Each pass yields q, then (lambda() => q + 1)(), then q + 100: sum over k in 0..199
//!   = 19900 + 20100 + 39900 = 79900 (the last is 19900 + 100 * 200).
module Conf;
import System;
public class It
  public static method Each(n: int): IEnumerable<int>
    var k := 0;
    while k < n do
      var q := k;
      yield return q;
      yield return (lambda() => q + 1)();
      yield return q + 100;
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
