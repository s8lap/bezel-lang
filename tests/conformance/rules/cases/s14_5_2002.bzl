//! case: s14_5_2002
//! doc: spec
//! section: 14.5
//! title: Math.Max with a string and a Guid argument is an inference
//!   disagreement
//! rule: All bindings a type parameter receives — across arguments, lambda
//!   returns, and the target — must be the **same type**.
//! cites: 2.2
//! expect: reject
//! diagnostic: type parameter
module Conf;

public class Program
  public static method Main()
    var g := Guid.Parse("11111111-1111-1111-1111-111111111111");
    var m := Math.Max("x", g);
  end Main;
end Program;
