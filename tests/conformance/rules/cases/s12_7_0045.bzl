//! case: s12_7_0045
//! doc: spec
//! section: 12.7
//! title: a type-test-narrowed nullable value type formats its payload, not the carrier
//! rule: When the test succeeds and the tested expression is one of
//!   [§7.4](#sec-narrowing-applies-to-locals-only)'s narrowing carriers, the
//!   carrier is **flow-narrowed** to `T` within the controlled block:
//! cites: 12.7, 7.4, 9.1, 2.18
//! expect: run
//! issue: 7319
//! stdout: 00005

module Conf;

public class Program
  public static method Main()
    var x: int? := 5;
    if x is int then
      Console.WriteLine(x.ToString("D5"));
    end;
  end Main;
end Program;
