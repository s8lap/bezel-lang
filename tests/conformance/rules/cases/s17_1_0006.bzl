//! case: s17_1_0006
//! doc: spec
//! section: 17.1
//! title: shift binds tighter than range — worked example
//! rule: "`1<<2..1<<10` parses as `(1<<2)..(1<<10)`" — Range looser than
//!   Shift.
//! cites: 17.1, 17.2
//! expect: run
//! stdout: 1024
module Conf;

public class Program
  public static method Main()
    var r := 1 << 2 .. 1 << 10;
    var e := r.End!;
    Console.WriteLine(e);
  end Main;
end Program;
