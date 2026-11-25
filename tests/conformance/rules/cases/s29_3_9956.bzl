//! case: s29_3_9956
//! doc: spec
//! section: 29.3
//! title: a lambda-bearing yield inside try/finally sees the finally's write to the captured local
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice versa.
//! cites: 29.3, 29.6, 27.5
//! expect: run
//! output: 40200
//! notes: Pins #9166 (try/finally shape; spec 29.6 admits yield in a try with only a finally). Each pass
//!   yields (lambda() => q + 1)() = k + 1, then on resumption the finally sets q := q + 1 = k + 1,
//!   and the trailing yield returns q = k + 1. Total 2 * 20100 = 40200.
module Conf;
import System;
public class It
  public static method Each(n: int): IEnumerable<int>
    var k := 0;
    while k < n do
      var q := k;
      try
        yield return (lambda() => q + 1)();
      finally
        q := q + 1;
      end try;
      yield return q;
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
