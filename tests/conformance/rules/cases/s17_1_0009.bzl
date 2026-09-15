//! case: s17_1_0009
//! doc: spec
//! section: 17.1
//! title: null-coalescing binds tighter than logical or
//! rule: "`??` binds tighter than `and`/`or`, so `a ?? b or c` parses as
//!   `(a ?? b) or c`."
//! cites: 17.1, 17.2
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a: bool? := false;
    var b := true;
    var c := true;
    var r := a ?? b or c;
    Console.WriteLine(r);
  end Main;
end Program;
