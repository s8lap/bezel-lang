//! case: s1_4_0004
//! doc: spec
//! section: 1.4
//! title: Exception is a PascalCase name, not a keyword
//! rule: Type identifiers like `Object`, `Array`, and `Exception` are not
//!   keywords; they are PascalCase names defined in the standard library.
//! expect: run
//! stdout: shadowed
module Conf;

public class Exception
  public static method Greet()
    Console.WriteLine("shadowed");
  end Greet;
end Exception;

public class Program
  public static method Main()
    Exception.Greet();
  end Main;
end Program;
