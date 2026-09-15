//! case: l2_11_0002
//! doc: stdlib
//! section: 2.11
//! title: new Subscription() is rejected -- no public constructor
//! rule: Subscription has no public constructor; it is obtained only from a
//!   registration call.
//! expect: reject
//! diagnostic: constructor
module Conf;

public class Program
  public static method Main()
    var s := new Subscription();
  end Main;
end Program;
