//! case: s27_6_0037
//! doc: spec
//! section: 27.6
//! title: a reference to Math.Sign written with its type-argument list is formed and invoked
//! rule: "a method reference over a generic method … names its type arguments on the member name
//!   … under §14.5's all-or-nothing rule for explicit lists" (§27.6, v0.350). The accepting twin
//!   of s27_6_0036: `Math.Sign<int>` supplies the list, and `int` satisfies `where T: INumber<T>`
//!   (stdlib §2.2, v0.350). Sign yields -1, 0 or 1 (stdlib §2.2).
//! cites: 27.6, 14.5
//! expect: run
//! output: -1
//!   0
//!   1
module Conf;

public class Program
  public static method Main()
    var g: (int) -> int := Math.Sign<int>;
    Console.WriteLine(g(-5));
    Console.WriteLine(g(0));
    Console.WriteLine(g(7));
  end Main;
end Program;
