//! case: s29_3_9953
//! doc: spec
//! section: 29.3
//! title: a lambda on the then arm of a yielded conditional expression leaves the local readable
//! rule: `yield return expression;` produces the next value and suspends.
//! cites: 29.3, 27.5, 20
//! expect: run
//! output: 70400
//! notes: Pins #9166 (conditional boxing: the lambda is formed on one arm only). Even k yield (lambda()
//!   => q + 1)() = k + 1; odd k yield 7; every pass then yields q * 3. Even k in 0..198 (100 of
//!   them): sum(k) = 9900, so 9900 + 100 = 10000. Odd k (100 of them): 700. Then 3 * 19900 = 59700.
//!   Total 10000 + 700 + 59700 = 70400.
module Conf;
import System;
public class It
  public static method Each(n: int): IEnumerable<int>
    var k := 0;
    while k < n do
      var q := k;
      yield return (if k mod 2 = 0 then (lambda() => q + 1)() else 7);
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
