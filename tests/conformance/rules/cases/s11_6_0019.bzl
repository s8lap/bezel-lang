//! case: s11_6_0019
//! doc: spec
//! section: 11.6
//! title: a nested generic type under a non-generic enclosing type is named from outside
//! rule: A nested type none of whose enclosing types is generic is referenced from outside as
//!   `Outer.Nested` (§11.6, v0.340); the nested type here is itself generic, so A.5's
//!   `TypeNoNullable` takes its type arguments after the last segment: `E.G<int>`.
//! cites: 11.6, 2.2
//! expect: run
//! stdout: 7
module Conf;

public class E
  public class G<T>
    public method V(): int => 7;
  end G;
end E;

public class Program
  public static method Main(): int
    using var g: E.G<int> := new E.G<int>();
    Console.WriteLine(g.V());
    return 0;
  end Main;
end Program;
