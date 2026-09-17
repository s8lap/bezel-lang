//! case: s17_7_0011
//! doc: spec
//! section: 17.7
//! title: a builtin positional-only record rejects the undeclared new T()
//! rule: "For a builtin record whose only declared constructor is its
//!   positional primary constructor (`ExceptionInfo` ...), the undeclared
//!   parameterless form `new T()` is rejected the same way — exactly as for
//!   an identically-shaped user record."
//! cites: 17.7, 31.4
//! expect: reject
//! diagnostic: constructor
module Conf;

public class Program
  public static method Main()
    var info := new ExceptionInfo();
  end Main;
end Program;
