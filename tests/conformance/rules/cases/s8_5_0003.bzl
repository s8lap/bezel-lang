//! case: s8_5_0003
//! doc: spec
//! section: 8.5
//! title: an enum that declares ToString to return NameOf renders the constant's name
//! rule: An enum may replace its default `ToString` by declaring a
//!   `ToString(): string` member; the replacement is used by every consumer,
//!   including interpolation. An enum whose rendering should be the constant's
//!   name declares `ToString` to return `E.NameOf(this)`.
//! cites: 8.5, 10.4, 10.5
//! expect: run
//! stdout: Low
module Conf;

enum Priority
  Low, Medium, High;

  public override method ToString(): string => Priority.NameOf(this);
end Priority;

public class Program
  public static method Main()
    Console.WriteLine($"{Priority.Low}");
  end Main;
end Program;
