//! case: l2_15_0009
//! doc: stdlib
//! section: 2.15
//! title: ReadFloat raises SerializationException on a point with no digits after it
//! rule: `ReadFloat` reads a JSON number of any length, its value rounded once to
//!   `double` as [§9.2](#sec-double-and-float) rounds `double.Parse`'s input; a
//!   number whose value, so rounded, is not finite (`1e999`, `-1e999`) raises
//!   `SerializationException` located at the number, so a document never reads
//!   back a non-finite value `WriteFloat` could not have written.
//! cites: 2.15, 9.2
//! expect: run
//! issue: 7341
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
