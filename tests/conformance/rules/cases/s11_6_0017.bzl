//! case: s11_6_0017
//! doc: spec
//! section: 11.6
//! title: inside a generic type a non-generic type enclosing a nested type below it is reached
//! rule: In a type position, therefore, no name passes through a generic enclosing type: a type nested
//!   in a generic type, at any depth, is named only where §2.7's enclosing-type lookup reaches it,
//!   or reaches a non-generic type enclosing it below the generic one (`Node<int>` in `Tree`'s own
//!   declaration or in the body of a type deriving from `Tree`; `Inner.Leaf` there for a `Leaf`
//!   nested in a non-generic `Inner`) (§11.6, v0.340).
//! cites: 11.6, 2.7
//! expect: run
//! stdout: 7
module Conf;

public class C<T>
  public class Inner
    public class Leaf
      public method Get(): int => 7;
    end Leaf;
  end Inner;

  public static method Run(): int
    using var l: Inner.Leaf := new Inner.Leaf();
    return l.Get();
  end Run;
end C;

public class Program
  public static method Main(): int
    Console.WriteLine(C<int>.Run());
    return 0;
  end Main;
end Program;
