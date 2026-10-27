//! case: l9_2_0009
//! doc: stdlib
//! section: 9.2
//! title: double.TryParse returns false exactly where Parse raises
//! rule: `TryParse` returns `false` in every case where `Parse` would raise, and
//!   the span forms carry the string forms' contract.
//! cites: 9.2
//! expect: run
//! stdout: False

module Conf;

public class Program
  public static method Main()
    var ok := double.TryParse("abc", out var v);
    Console.WriteLine(ok);
  end Main;
end Program;
