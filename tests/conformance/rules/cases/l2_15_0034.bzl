//! case: l2_15_0034
//! doc: stdlib
//! section: 2.15
//! title: nested arrays are read element by element with HasMoreElements
//! rule: A JSON array; the `elementCount` argument is likewise advisory. On the
//!   read side `JsonDeserializer.BeginArray()` returns `-1`, so consumers use
//!   `HasMoreElements()`.
//! cites: 2.15
//! expect: run
//! output: OK (1;2;)(3;)
//!   OK ()()
//!   SER
//!   SER
//! notes: RFC 8259 (the JSON the mapping names) defines the grammar. The third input
//!   has no comma between the inner arrays; the fourth doubles it.
module Conf;

import System.Serialization;

public class Program
  public static method Main()
    Console.WriteLine(LL("[[1, 2], [3]]"));
    Console.WriteLine(LL("[[], []]"));
    Console.WriteLine(LL("[[1, 2] [3]]"));
    Console.WriteLine(LL("[[1, 2],, [3]]"));
  end Main;

  private static method LL(j: string): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j);
      _ := d.BeginArray();
      var acc: string := "";
      while d.HasMoreElements() do
        _ := d.BeginArray();
        acc := acc + "(";
        while d.HasMoreElements() do
          acc := acc + d.ReadInt().ToString() + ";";
        end;
        d.EndArray();
        acc := acc + ")";
      end;
      d.EndArray();
      r := "OK " + acc;
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end LL;
end Program;
