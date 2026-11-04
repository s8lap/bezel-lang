//! case: l2_15_0029
//! doc: stdlib
//! section: 2.15
//! title: a JSON array does not admit a doubled or a leading comma
//! rule: Malformed input, type mismatches (a `ReadInt` when the next token is a
//!   string), and depth-limit violations raise `SerializationException`
//! cites: 2.15
//! expect: run
//! output: OK 1 2
//!   SER
//!   SER
//! notes: RFC 8259 (the JSON the mapping names) defines the grammar. A comma must be
//!   followed by an element and must follow one, so a doubled comma and a
//!   leading comma are malformed.
module Conf;

import System.Serialization;

record RA(V: [2]int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(Try("{\"V\": [1, 2]}"));
    Console.WriteLine(Try("{\"V\": [1,,2]}"));
    Console.WriteLine(Try("{\"V\": [,1, 2]}"));
  end Main;

  private static method Try(j: string): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j);
      var v := RA.Deserialize(d);
      r := $"OK {v.V[0]} {v.V[1]}";
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end Try;
end Program;
