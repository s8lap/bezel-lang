//! case: s28_3_0029
//! doc: spec
//! section: 28.3
//! title: a fresh owned value returned through a plain class-typed return is rejected
//! rule: An instance method whose declared return type is a plain concrete class type
//!   may return only a receiver-sourced borrow; a fresh owned value is rejected —
//!   declare `owned`.
//! cites: 28.6
//! expect: reject
//! diagnostic: owned
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Peek(): Widget
    var w := new Widget();
    return w;
  end Peek;
end Program;
