//! case: s29_3_9954
//! doc: spec
//! section: 29.3
//! title: a lambda on the else arm of a yielded conditional expression leaves the local readable
//! rule: `yield return expression;` produces the next value and suspends.
//! cites: 29.3, 27.5, 20
//! expect: run
//! output: 70500
//! notes: Pins #9166, the mirror of s29_3_9953. Even k yield 7 (100 passes: 700); odd k yield (lambda()
//!   => q + 1)() = k + 1: odd k in 1..199 sum to 10000, plus 100 = 10100. Then 3 * 19900 = 59700.
//!   Total 700 + 10100 + 59700 = 70500.
module Conf;
import System;
public class It
  public static method Each(n: int): IEnumerable<int>
    var k := 0;
    while k < n do
      var q := k;
      yield return (if k mod 2 = 0 then 7 else (lambda() => q + 1)());
      yield return q * 3;
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
