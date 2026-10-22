//! case: s11_6_0005
//! doc: spec
//! section: 11.6
//! title: a type nested in a generic type is not named through its bare generic enclosing
//!   segment in new
//! rule: "A bare enclosing segment fixes arity 0 ... In a type position, therefore, no name
//!   passes through a generic enclosing type: a type nested in a generic type, at any depth,
//!   is named only where §2.7's enclosing-type lookup reaches it" (§11.6, v0.340). `new
//!   C.N()` names `N` through the bare generic `C`.
//! cites: 2.7, 14.1
//! expect: reject
//! diagnostic: generic-argument count
module Conf;

public class C<T>
  public class N
    public method V(): int
      return 7;
    end V;
  end N;
end C;

public class Program
  public static method Main()
    using var n := new C.N();
    Console.WriteLine(n.V());
  end Main;
end Program;
