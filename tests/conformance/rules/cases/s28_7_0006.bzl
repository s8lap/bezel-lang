//! case: s28_7_0006
//! doc: spec
//! section: 28.7
//! title: reassigning an owned local while a live local borrow of it exists is rejected
//! rule: Reassigning a plain-var owned local while a live local borrow of
//!   its current value exists is rejected by the consume-site freeze — the
//!   implicit disposal would silently invalidate the borrow.
//! cites: 28.7, 28.3
//! expect: reject
//! diagnostic: live
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
  end Ping;
end Widget;

public class Program
  public static method Main()
    var w := new Widget();
    var b := w;
    w := new Widget();
    b.Ping();
  end Main;
end Program;
