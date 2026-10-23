//! case: s35_3_0037
//! doc: spec
//! section: 35.3
//! title: the same generic payload-free case written as a value is admitted
//! rule: For a generic choice the type arguments are written at the construction
//!   (Result<int, string>.Ok(value)); a payload-free case is a value expression, not a call.
//! cites: 14.5
//! expect: run
//! output: -1
module Conf;

public choice Opt<T>
  Yes(v: T),
  No;
end Opt;

public class Program
  public static method Main()
    var o := Opt<int>.No;
    Console.WriteLine((case o of Yes(var v) => v, No => -1));
  end Main;
end Program;
