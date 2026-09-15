//! case: s5_1_0011
//! doc: spec
//! section: 5.1
//! title: an enum does not implicitly convert to its underlying integer type
//! rule: The following are never implicit: between an enum and its underlying
//!   integer.
//! cites: 5.1
//! expect: reject
//! diagnostic: never implicit
module Conf;

public enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Convert(p: Priority)
    var n: int := p;
  end Convert;
end Program;
