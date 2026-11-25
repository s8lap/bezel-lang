//! case: s29_3_9951
//! doc: spec
//! section: 29.3
//! title: a yield operand invoking a nested lambda leaves the captured local readable afterwards
//! rule: `yield return expression;` produces the next value and suspends.
//! cites: 29.3, 27.5
//! expect: run
//! output: 41000
//! notes: Pins #9166 / #9281. Each pass yields (lambda() => (lambda() => q + 1)())() = k + 1, then q +
//!   5. Sum over k in 0..199: 20100 + (19900 + 5 * 200) = 20100 + 20900 = 41000.
module Conf;
import System;
public class It
  public static method Each(n: int): IEnumerable<int>
    var k := 0;
    while k < n do
      var q := k;
      yield return (lambda() => (lambda() => q + 1)())();
      yield return q + 5;
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
