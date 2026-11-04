//! case: l2_15_0025
//! doc: stdlib
//! section: 2.15
//! title: a JSON object's members must be separated by a comma: around a nested
//!   object
//! rule: Malformed input, type mismatches (a `ReadInt` when the next token is a
//!   string), and depth-limit violations raise `SerializationException`
//! cites: 2.15
//! expect: run
//! output: OK 1 2 3
//!   SER
//!   SER
//! notes: RFC 8259 (the JSON the mapping names) defines the grammar. The second
//!   input has no comma after the nested object; the third has none inside it.
module Conf;

import System.Serialization;

record RT(V: int, W: int): ISerializable, IDeserializable;
record RN(P: RT, K: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(Try("{\"P\": {\"V\": 1, \"W\": 2}, \"K\": 3}"));
    Console.WriteLine(Try("{\"P\": {\"V\": 1, \"W\": 2} \"K\": 3}"));
    Console.WriteLine(Try("{\"P\": {\"V\": 1 \"W\": 2}, \"K\": 3}"));
  end Main;

  private static method Try(j: string): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j);
      var v := RN.Deserialize(d);
      r := $"OK {v.P.V} {v.P.W} {v.K}";
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end Try;
end Program;
