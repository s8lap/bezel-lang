//! case: s28_7_0013
//! doc: spec
//! section: 28.7
//! title: reassigning an Object-typed owned local while a live local borrow of it exists is rejected
//! rule: Reassigning a plain-var owned local while a live local borrow of
//!   its current value exists is rejected by the consume-site freeze - the
//!   implicit disposal would silently invalidate the borrow. The rule names a
//!   LOCAL, not a class-typed local: the reader's static type does not decide
//!   whether a use-after-free is diagnosed.
//! cites: 28.7, 28.3
//! expect: reject
//! diagnostic: live
//! issue: 7103
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
  end Ping;
end Widget;

public class Program
  public static method Main()
    var w: Object := new Widget();
    var b: Object := w;
    w := 7;
    var n := b.GetHashCode();
    _ := n;
  end Main;
end Program;
