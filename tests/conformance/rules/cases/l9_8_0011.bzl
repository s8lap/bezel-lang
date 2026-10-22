//! case: l9_8_0011
//! doc: stdlib
//! section: 9.8
//! title: a user generic class named Range<T> keeps its own Contains, called directly and through a lambda
//! rule: Names are compared with their generic arity (§2.2 *Collisions*, v0.340) and a current-module declaration is found
//!   before an import (§2.7 rule 1), so the module's own `Range<T>` hides the always-available stdlib `Range<T>` and
//!   `r.Contains(2)` calls the user's method, which returns `int`, not `bool`.
//! cites: 2.2, 2.7
//! expect: run
//! output: 42
//!   42
module Conf;

public class Range<T>
  public method Contains(x: T): int return 42; end Contains;
end Range;

public class Program
  public static method Main()
    var r := new Range<int>();
    var f := lambda() => r.Contains(2);
    Console.WriteLine(f());
    var n: int := r.Contains(3);
    Console.WriteLine(n);
  end Main;
end Program;
