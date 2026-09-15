//! case: s28_10_0008
//! doc: spec
//! section: 28.10
//! title: reassigning a using var of runtime-managed type is still a compile error
//! rule: using var is a scope pin — it is implicitly non-reassignable,
//!   whatever the right-hand side's type kind; reassigning a using var
//!   variable is a compile error.
//! cites: 28.10
//! expect: reject
//! diagnostic: reassign
module Conf;

public class Program
  public static method Main()
    using var s := "hello";
    s := "world";
  end Main;
end Program;
