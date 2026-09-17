//! case: s15_4_0006
//! doc: spec
//! section: 15.4
//! title: a fresh non-trivial-dispose element in a borrowing-family literal is rejected at the literal site
//! rule: For borrowing-family targets, each element expression of
//!   non-trivial-dispose class type must be a borrow; a fresh producer of
//!   such a type is rejected at the literal site (§15.4).
//! cites: 15.4, 28.9
//! expect: reject
//! diagnostic: literal
module Conf;

public class Connection
  public constructor()
  end constructor;

  destructor
  end destructor;
end Connection;

public class Program
  public static method Main()
    using var conns: Array<Connection> := [new Connection()];
  end Main;
end Program;
