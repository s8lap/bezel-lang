//! case: s5_1_0140
//! doc: spec
//! section: 5.1
//! title: a uint argument is not implicitly convertible to an int parameter
//! rule: Between signed and unsigned types of the same or narrower width (e.g.
//!   `int → uint`, `uint → int`, `short → ushort`) is never implicit; a `uint`
//!   is the same width as `int`, and the stdlib declares the parameter as
//!   `int`: `Substring(startIndex: int)`.
//! cites: 5.1, stdlib 9.6
//! expect: reject
//! diagnostic: never implicit
module Conf;

public class Program
  public static method Main(): int
    var s: string := "c" + "x".Repeat(250) + "c";
    var v: uint := 200;
    Console.WriteLine(s.Substring(v).Length.ToString());
    return 0;
  end Main;
end Program;
