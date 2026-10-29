//! case: s14_2_0092
//! doc: spec
//! section: 14.2
//! title: the non-nullable Math.Max<int> reference meets the interface bound and runs
//! rule: "a class or interface bound admits only non-nullable substitutions, as `where T: class`
//!   does" (§14.2, v0.350). The accepting twin of s14_2_0091: `int` is non-nullable, and
//!   `Math.Max<T>` declares `where T: IComparable<T>` (stdlib §2.2, v0.350), which `int` meets.
//! cites: 14.2, 27.6
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var m: (int, int) -> int := Math.Max<int>;
    Console.WriteLine(m(3, 5));
  end Main;
end Program;
