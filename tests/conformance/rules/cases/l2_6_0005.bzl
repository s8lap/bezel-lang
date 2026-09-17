//! case: l2_6_0005
//! doc: stdlib
//! section: 2.6
//! title: new Guid(...) is rejected -- Guid declares no constructor
//! rule: Guid declares no constructor, so new Guid(...) is a compile error at
//!   any argument list.
//! expect: reject
//! diagnostic: constructor
module Conf;

public class Program
  public static method Main()
    var g := new Guid();
  end Main;
end Program;
