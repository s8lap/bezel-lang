//! case: l2_15_0033
//! doc: stdlib
//! section: 2.15
//! title: a malformed separator in an array read with HasMoreElements raises
//!   SerializationException
//! rule: Malformed input, type mismatches (a `ReadInt` when the next token is a
//!   string), and depth-limit violations raise `SerializationException`
//! cites: 2.15
//! expect: run
//! output: OK -1 [1;2;]
//!   SER
//!   SER
//!   SER
//! notes: RFC 8259 (the JSON the mapping names) defines the grammar. The three
//!   malformed inputs lack a comma, double one, and lead with one; each is
//!   malformed wherever the reader notices it, so each raises inside the try.
module Conf;

import System.Serialization;

public class Program
  public static method Main()
    Console.WriteLine(L("[1, 2]"));
    Console.WriteLine(L("[1 2]"));
    Console.WriteLine(L("[1,,2]"));
    Console.WriteLine(L("[,1]"));
  end Main;

  private static method L(j: string): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j);
      var n := d.BeginArray();
      var acc: string := "";
      while d.HasMoreElements() do
        acc := acc + d.ReadInt().ToString() + ";";
      end;
      d.EndArray();
      r := $"OK {n} [{acc}]";
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end L;
end Program;
