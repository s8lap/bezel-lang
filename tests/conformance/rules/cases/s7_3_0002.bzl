//! case: s7_3_0002
//! doc: spec
//! section: 7.3
//! title: assigning null to a nullable local is admitted
//! rule: Assigning `null` to a non-nullable target is a compile error
//!   (contrapositive: a nullable target admits it).
//! cites: 18.5
//! expect: run
//! stdout: null
module Conf;

public class Program
  public static method Main()
    var name: string? := null;
    Console.WriteLine($"{name}");
  end Main;
end Program;
