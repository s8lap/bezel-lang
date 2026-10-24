//! case: s5_1_0143
//! doc: spec
//! section: 5.1
//! title: a char index converts to an int index parameter
//! rule: The conversion table lists `int` among the implicit targets of
//!   `char`.
//! cites: 5.1
//! expect: run
//! stdout: 20
module Conf;

public class Program
  public static method Main(): int
    var a: Array<int> := [10, 20, 30];
    var c: char := char(1);
    Console.WriteLine(a[c]);
    return 0;
  end Main;
end Program;
