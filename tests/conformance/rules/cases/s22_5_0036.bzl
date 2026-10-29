//! case: s22_5_0036
//! doc: spec
//! section: 22.5
//! title: ArgumentException and TimeoutException accept their constructor arguments by name in either order
//! rule: Each supplied argument binds exactly one parameter by name (spec 22.5). `ArgumentException` declares
//!   `(message, paramName)` (spec 31.4) and `TimeoutException` declares `(message, inner)` (stdlib 6.1), so
//!   the written order of the names does not change which value lands where.
//! cites: 22.5, 31.4
//! expect: run
//! output: mm|pp
//!   tm
module Conf;

public class Program
  public static method Main()
    using var e := new ArgumentException(paramName := "pp", message := "mm");
    Console.WriteLine($"{e.Message}|{e.ParamName}");
    using var inner := new Exception("in");
    using var t := new TimeoutException(inner := inner, message := "tm");
    Console.WriteLine(t.Message);
  end Main;
end Program;
