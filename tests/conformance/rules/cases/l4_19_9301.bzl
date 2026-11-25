//! case: l4_19_9301
//! doc: stdlib
//! section: 4.19
//! title: a lambda over a captured FrozenArray reads Length as an int
//! rule: public readonly property Length: int;
//! cites: 4.19, 27.3
//! expect: run
//! output: 3
//!   4
//!   True
//! notes: Harvested from the #9185 review (p1, first half). Source [7, 2, 5], so `Length` is 3. The lambdas
//!   capture `f`; an expression-bodied lambda's value type is its body's type (spec 27.3), here `int`, so
//!   `fn()` is 3, `fn2()` is 3 + 1 = 4, and `fn3()` is 3 * 2 > 5, True (spec 8.5 renders bool as True).
module Conf;
public class Program
  public static method Main(): int
    using var s := new Array<int>();
    s.Add(7); s.Add(2); s.Add(5);
    using var f := FrozenArray<int>.From(s);
    var fn := lambda() => f.Length;
    var fn2 := lambda() => f.Length + 1;
    var fn3 := lambda() => f.Length * 2 > 5;
    Console.WriteLine(fn());
    Console.WriteLine(fn2());
    Console.WriteLine(fn3());
    return 0;
  end Main;
end Program;
