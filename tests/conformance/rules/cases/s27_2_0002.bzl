//! case: s27_2_0002
//! doc: spec
//! section: 27.2
//! title: a null-conditional call on a null function value is a no-op yielding null
//! rule: `((int) -> bool)?` is a nullable function returning bool; the
//!   null-conditional call result is nullable, and a null hole renders the
//!   four characters `null` (§18.5).
//! cites: 27.2, 27.7, 18.5
//! expect: run
//! stdout: null
module Conf;

public class Program
  public static method Main()
    var check: ((int) -> bool)? := null;
    var result := check?(5);
    Console.WriteLine($"{result}");
  end Main;
end Program;
