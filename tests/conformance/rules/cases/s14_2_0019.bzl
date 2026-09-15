//! case: s14_2_0019
//! doc: spec
//! section: 14.2
//! title: a member where clause naming the enclosing type parameter is rejected
//! rule: A `where` clause on a member declaration may constrain only that
//!   member's own type parameters; a clause whose constrained parameter is a
//!   type parameter of the enclosing declaration is a compile error (v0.277).
//! cites: 14.2
//! expect: reject
//! diagnostic: own type parameters
module Conf;

public class Container<T>
  public method Foo() where T: notnull
    Console.WriteLine("x");
  end Foo;

  public constructor()
  end constructor;
end Container;
