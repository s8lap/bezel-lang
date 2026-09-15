//! case: l4_19_0002
//! doc: stdlib
//! section: 4.19
//! title: FrozenArray<T>.From(items) copies and is the only construction form for string
//! rule: static method From(items: Array<T>): owned FrozenArray<T>; //
//!   Copies; the only construction form for T = string.
//! cites: 4.19
//! expect: run
//! stdout: b
module Conf;

public class Program
  public static method Main()
    using var xs: Array<string> := ["a", "b"];
    using var fa := FrozenArray<string>.From(xs);
    Console.WriteLine(fa[1]);
  end Main;
end Program;
