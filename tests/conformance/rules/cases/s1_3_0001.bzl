//! case: s1_3_0001
//! doc: spec
//! section: 1.3
//! title: an identifier may begin with a Unicode letter
//! rule: An identifier begins with a Unicode letter or underscore (`_`) and
//!   continues with letters, digits, or underscores.
//! expect: run
//! stdout: 7
module Conf;

public class Program
  public static method Main()
    var émission := 7;
    Console.WriteLine($"{émission}");
  end Main;
end Program;
