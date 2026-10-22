//! case: s11_6_0006
//! doc: spec
//! section: 11.6
//! title: a nested type is not named through a bare generic enclosing segment in an annotation
//! rule: "A bare enclosing segment fixes arity 0 ... In a type position, therefore, no name
//!   passes through a generic enclosing type: a type nested in a generic type, at any depth,
//!   is named only where §2.7's enclosing-type lookup reaches it" (§11.6, v0.340). The
//!   annotation `C.N?` is a type position that passes through the generic `C`.
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
    var n: C.N? := null;
    Console.WriteLine(n = null);
  end Main;
end Program;
