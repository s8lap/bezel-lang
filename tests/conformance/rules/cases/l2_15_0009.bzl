//! case: l2_15_0009
//! doc: stdlib
//! section: 2.15
//! title: ReadFloat raises SerializationException on a point with no digits after it
//! rule: **Errors.** Malformed input, type mismatches (a `ReadInt` when the
//!   next token is a string), and depth-limit violations raise
//!   `SerializationException` (declared in `System.Serialization`); a JSON
//!   number has the shape the input grammar of
//!   [§9.2](#sec-double-and-float) admits, and that grammar admits a `.` only
//!   when one or more digits follow it.
//! cites: 2.15, 9.2
//! expect: run
//! output: SEREX
//!   1500
//!   SEREX
//!   SEREX

module Conf;

import System.Serialization;

public class Program
  public static method Main()
    Show("1.e3");
    Show("1.5e3");
    Show("1.");
    Show("1.e");
  end Main;

  private static method Show(s: string)
    try
      using var d := new JsonDeserializer(s);
      Console.WriteLine(d.ReadFloat());
    catch
      SerializationException => Console.WriteLine("SEREX");
    end;
  end Show;
end Program;
