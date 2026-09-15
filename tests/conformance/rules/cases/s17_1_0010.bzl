//! case: s17_1_0010
//! doc: spec
//! section: 17.1
//! title: equality binds tighter than null-coalescing
//! rule: Precedence table: Equality (`=` `<>`) is level 11, Null-coalescing
//!   (`??`) is level 12 — the lower-numbered level binds tighter, so
//!   `x ?? y = z` parses as `x ?? (y = z)`.
//! cites: 17.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var x: bool? := null;
    var y := 3;
    var z := 3;
    var r := x ?? y = z;
    Console.WriteLine(r);
  end Main;
end Program;
