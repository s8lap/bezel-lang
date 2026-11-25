//! case: s29_3_9950
//! doc: spec
//! section: 29.3
//! title: a yield operand invoking a lambda over a loop-body local leaves the local readable afterwards
//! rule: `yield return expression;` produces the next value and suspends.
//! cites: 29.3, 27.5
//! expect: run
//! output: 59900
//! notes: Pins #9166 / #9281. Each pass k binds q := k, yields (lambda() => q + 1)() = k + 1 and then q
//!   * 2. Sum over k in 0..199: (20100) + (2 * 19900) = 20100 + 39800 = 59900. The lambda is
//!   immediately invoked (spec 28.3: an invoked closure may capture a loop-body local).
module Conf;
import System;
public class It
  public static method Each(n: int): IEnumerable<int>
    var k := 0;
    while k < n do
      var q := k;
      yield return (lambda() => q + 1)();
      yield return q * 2;
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
