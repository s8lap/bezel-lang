//! case: s35_2_0008
//! doc: spec
//! section: 35.2
//! title: a declared homogeneous `=` overload on a choice replaces the generated one
//! rule: A choice body may declare operator overloads; records, enums, and choices may
//!   overload = and <>, and a homogeneous overload disqualifies the choice from
//!   auto-hashable, §25.2's co-declaration rule then supplying the equality
//!   surface.
//! cites: 35.2, 35.7, 25.2
//! expect: run
//! output: True
//!   True
module Conf;

public choice Priority
  Low,
  High;

  // Deliberately not case-discriminating: auto-equality would answer False on the
  // first line below, so the output shows the declared operator is what runs.
  public static method operator =(a: Priority, b: Priority): bool => true;
  public static method operator <>(a: Priority, b: Priority): bool => false;
  public method Equals(other: Priority): bool => true;
  public method GetHashCode(): int => 0;
end Priority;

public class Program
  public static method Main()
    Console.WriteLine(Priority.Low = Priority.High);
    Console.WriteLine(Priority.Low = Priority.Low);
  end Main;
end Program;
