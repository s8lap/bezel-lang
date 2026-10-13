//! case: s28_6_0037
//! doc: spec
//! section: 28.6
//! title: a this-returning chain over a new root is a borrow of a temporary, not an owned value
//! rule: When a chain originates from a `new` expression or another owned
//!   producer ... the root is an **unbound owned temporary** (§28.3) and every
//!   `this`-returning call on it is a borrow of that temporary ... bound to a
//!   `var`, stored, returned, or passed to an `owned` parameter it is rejected
//!   as a borrow of a temporary (K8.4) — `Keep(new Pool().First())` lends
//!   `Pool`'s inner object from a temporary `Pool` and is a compile error at
//!   the argument.
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: temporary
module Conf;

public class Inner
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("inner disposed");
  end destructor;
end Inner;

public class Pool
  private owned _first: Inner := new Inner();

  public method First(): Inner => _first;
end Pool;

public class Demo
  public static method Keep(owned i: Inner)
  end Keep;

  public static method Run()
    Keep(new Pool().First());
  end Run;
end Demo;
