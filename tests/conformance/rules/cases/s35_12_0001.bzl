//! case: s35_12_0001
//! doc: spec
//! section: 35.12
//! title: a borrow-bearing choice is never sendable
//! rule: Borrow-bearing choices are never sendable (they carry origin-domain
//!   borrows).
//! expect: reject
//! diagnostic: sendable
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Holding
  Some(widget: Widget),   // plain class payload: borrow-bearing
  None;
end Holding;

public class Checker<T> where T: sendable
  public static method Check(): bool => true;
end Checker;

public class Program
  public static method Main()
    Console.WriteLine(Checker<Holding>.Check());   // ERROR: a borrow-bearing choice is never sendable
  end Main;
end Program;
