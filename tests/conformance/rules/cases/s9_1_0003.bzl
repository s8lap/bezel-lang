//! case: s9_1_0003
//! doc: spec
//! section: 9.1
//! title: positional-record equality compares all positional fields
//! rule: Value equality (=, <>) is based on all positional fields (those marked
//!   equality skip excepted), generated only when every participating positional
//!   field is of an admissible component type.
//! expect: run
//! stdout: True,True
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Main()
    var a := new Point(1, 2);
    var b := new Point(1, 2);
    var c := new Point(1, 3);
    Console.WriteLine($"{a = b},{a <> c}");
  end Main;
end Program;
