//! case: l9_8_0010
//! doc: stdlib
//! section: 9.8
//! title: a user record named Range keeps its own Contains, whatever its result type
//! rule: A non-generic `Range` is a different name from the always-available `Range<T>` (names are compared with their
//!   generic arity, §2.2 *Collisions*, v0.340), and a current-module declaration is found before an import (§2.7 rule 1),
//!   so `r.Contains(2)` calls the record's own `Contains`, which returns `int` (`x + Lo`) and is not typed `bool`.
//! cites: 2.2, 2.7
//! expect: run
//! output: 6
//!   5
module Conf;

public record Range(Lo: int, Hi: int)
  public method Contains(x: int): int return x + Lo; end Contains;
end Range;

public class Program
  public static method Main()
    var r := new Range(4, 9);
    var f := lambda() => r.Contains(2);
    Console.WriteLine(f());
    var n: int := r.Contains(1);
    Console.WriteLine(n);
  end Main;
end Program;
