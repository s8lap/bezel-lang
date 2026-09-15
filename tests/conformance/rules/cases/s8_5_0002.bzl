//! case: s8_5_0002
//! doc: spec
//! section: 8.5
//! title: the default ToString for an enum renders the qualified type name, not the constant's name
//! rule: For enums, the compiler-generated default `ToString` returns the
//!   type's qualified name (the stdlib `FullName` rendering), not the
//!   constant's declared identifier. `NameOf` is the separate table that
//!   answers the constant's identifier (§10.4).
//! cites: 8.5, 10.4
//! expect: run
//! stdout: Conf.Priority
module Conf;

enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Main()
    Console.WriteLine($"{Priority.Low}");
  end Main;
end Program;
