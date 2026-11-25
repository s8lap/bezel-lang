//! case: s29_3_9955
//! doc: spec
//! section: 29.3
//! title: other iterator locals of several types keep their values across a lambda-bearing yield operand
//! rule: `yield return expression;` produces the next value and suspends.
//! cites: 29.3, 27.5
//! expect: run
//! output: 210300
//! notes: Pins #9166 (other suspend-locals survive the operand's re-spill). Locals d (double), name
//!   (string) and big (long) live across the whole loop. Pass i (0-based) yields q + 1 = i + 1,
//!   then big and d advance: d = 1.5 + (i + 1) exceeds 3.0 exactly for i >= 1 (199 passes, dd = 1);
//!   big = 10000000000 + (i + 1) exceeds 10000000050 exactly for i >= 50 (150 passes, bb = 1).
//!   Second yield = bb * 1000 + dd * 100 + 2 + q. Sum = 20100 + (150000 + 19900 + 400 + 19900) =
//!   20100 + 190200 = 210300.
module Conf;
import System;
public class It
  public static method Each(n: int): IEnumerable<int>
    var k := 0;
    var d := 1.5;
    var name := "ab";
    var big := 10000000000L;
    while k < n do
      var q := k;
      yield return (lambda() => q + 1)();
      big := big + 1L;
      d := d + 1.0;
      var dd := 0;
      if d > 3.0 then dd := 1; end;
      var bb := 0;
      if big > 10000000050L then bb := 1; end;
      yield return bb * 1000 + dd * 100 + name.Length + q;
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
