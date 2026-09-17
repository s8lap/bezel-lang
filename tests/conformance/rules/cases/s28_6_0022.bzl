//! case: s28_6_0022
//! doc: spec
//! section: 28.6
//! title: disposing a default-iteration element borrow is rejected
//! rule: No element-borrow disposal. An element borrow obtained from a
//!   borrowing-family collection — a default-iteration variable — is a
//!   borrow like any other: disposing it is a compile error, with no
//!   exception.
//! cites: 28.6, 28.11
//! expect: reject
//! diagnostic: element borrow
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Cleanup(pool: Array<Widget>)
    for var w in pool do
      w.Dispose();
    end;
  end Cleanup;
end Program;
