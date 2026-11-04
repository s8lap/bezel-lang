//! case: l2_15_0023
//! doc: stdlib
//! section: 2.15
//! title: a JSON object's members must be separated by a comma: two int members
//! rule: Malformed input, type mismatches (a `ReadInt` when the next token is a
//!   string), and depth-limit violations raise `SerializationException`
//! cites: 2.15
//! expect: run
//! output: OK 1 2
//!   OK 1 2
//!   SER
//!   SER
//! notes: RFC 8259 (the JSON the mapping names) defines the grammar. A JSON object
//!   separates its members with ',', so two members with only whitespace or
//!   nothing between them are not a JSON object and the input is malformed. The
//!   first two inputs are the accepting controls.
module Conf;

import System.Serialization;

record RT(V: int, W: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(Try("{\"V\": 1, \"W\": 2}"));
    Console.WriteLine(Try("{\"V\":1,\"W\":2}"));
    Console.WriteLine(Try("{\"V\": 1 \"W\": 2}"));
    Console.WriteLine(Try("{\"V\":1\"W\":2}"));
  end Main;

  private static method Try(j: string): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j);
      var v := RT.Deserialize(d);
      r := $"OK {v.V} {v.W}";
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end Try;
end Program;
