//! case: s29_3_9958
//! doc: spec
//! section: 29.3
//! title: a yield operand invoking a lambda over a using var local leaves the owned local readable
//!   afterwards
//! rule: `yield return expression;` produces the next value and suspends.
//! cites: 29.3, 27.5, 28.3
//! expect: run
//! output: 2800
//! notes: Pins #9166 (owned local captured by an invoked lambda in the operand; spec 28.3 admits a
//!   closure that is immediately invoked to capture an owned local read-only). Each pass binds a
//!   Res under using var, yields (lambda() => r.Id())() = 7 and then r.Id() = 7: 200 passes sum to
//!   2800.
module Conf;
import System;
public class Res
  public method Id(): int => 7;
end Res;
public class It
  public static method Each(n: int): IEnumerable<int>
    var k := 0;
    while k < n do
      using var r := new Res();
      yield return (lambda() => r.Id())();
      yield return r.Id();
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
