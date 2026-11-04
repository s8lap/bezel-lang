//! case: l2_15_0026
//! doc: stdlib
//! section: 2.15
//! title: a JSON object does not admit an empty member between two commas
//! rule: Malformed input, type mismatches (a `ReadInt` when the next token is a
//!   string), and depth-limit violations raise `SerializationException`
//! cites: 2.15
//! expect: run
//! output: OK 1 2
//!   SER
//! notes: RFC 8259 (the JSON the mapping names) defines the grammar. A comma must be
//!   followed by a member, so ',,' is malformed.
module Conf;

import System.Serialization;

record RT(V: int, W: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(Try("{\"V\": 1, \"W\": 2}"));
    Console.WriteLine(Try("{\"V\": 1,, \"W\": 2}"));
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
