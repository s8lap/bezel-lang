//! case: s21_10_0003
//! doc: spec
//! section: 21.10
//! title: a local record may not declare generic parameters
//! rule: The `GenericParams?` slot must be empty (v1) — a local type
//!   declares no type parameters of its own.
//! cites: 21.10
//! expect: reject
//! diagnostic: generic
module Conf;

public class Program
  public static method Main()
    record Box<T>(Value: T);
    var b := new Box<int>(5);
    Console.WriteLine(b.Value.ToString());
  end Main;
end Program;
