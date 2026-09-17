//! case: s34_0004
//! doc: spec
//! section: 34
//! title: a documented entry point compiles and runs unaffected
//! rule: The compiler preserves documentation comments for documentation
//!   generators but does not interpret their content — compilation and
//!   runtime behavior are unaffected.
//! expect: run
//! stdout: documented
module Conf;

public class Program
  /// <summary>
  /// Entry point.
  /// </summary>
  public static method Main()
    Console.WriteLine("documented");
  end Main;
end Program;
