//! case: l2_3_0016
//! doc: stdlib
//! section: 2.3
//! title: the value's own ToString is the other remedy for the ambiguous Convert.ToString
//! rule: stdlib §2.3: "the remedy is an explicit cast to one parameter type, or
//!     the value's own `ToString()`, which renders it in its own type". The
//!     `byte` value's own no-format `ToString()` renders it in its own type,
//!     so the `byte` is rendered without the ambiguous `Convert.ToString` call.
//! cites: 2.3, 9.1
//! expect: run
//! stdout: 200
module Conf;

public class Program
  public static method Main()
    var b: byte := 200;
    Console.WriteLine(b.ToString());
  end Main;
end Program;
