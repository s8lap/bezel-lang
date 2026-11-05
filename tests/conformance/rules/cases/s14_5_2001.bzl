//! case: s14_5_2001
//! doc: spec
//! section: 14.5
//! title: Math.Clamp with an int value and Guid bounds is an inference
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
    var m := Math.Clamp(1, g, g);
  end Main;
end Program;
