//! case: l2_15_0038
//! doc: stdlib
//! section: 2.15
//! title: an unknown field whose value ends the input early is malformed even when
//!   unknown fields are ignored
//! rule: Malformed input, type mismatches (a `ReadInt` when the next token is a
//!   string), and depth-limit violations raise `SerializationException`
//! cites: 2.15, 2.14
//! expect: run
//! output: OK 5
//!   SER
//!   SER
//!   SER
//! notes: Skipping consumes the value to its end; when the input stops before that
//!   end no skip can succeed and the document is truncated. The first input is
//!   the accepting control.
module Conf;

import System.Serialization;

record R1(V: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(Skip("{\"V\": 5, \"X\": {\"a\": 1}}", true));
    Console.WriteLine(Skip("{\"X\": {\"a\": [1, 2", true));
    Console.WriteLine(Skip("{\"X\": [1, 2", true));
    Console.WriteLine(Skip("{\"V\": 5, \"X\": {\"a\": ", true));
  end Main;

  private static method Skip(j: string, ignore: bool): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j, new JsonOptions(false, 2, false, ignore, 64));
      var v := R1.Deserialize(d);
      r := $"OK {v.V}";
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end Skip;
end Program;
