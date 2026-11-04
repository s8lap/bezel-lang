//! case: l2_15_0024
//! doc: stdlib
//! section: 2.15
//! title: a JSON object's members must be separated by a comma: string and bool
//!   members
//! rule: Malformed input, type mismatches (a `ReadInt` when the next token is a
//!   string), and depth-limit violations raise `SerializationException`
//! cites: 2.15
//! expect: run
//! output: OK a,b True
//!   SER
//!   SER
//!   OK [1 2] False
//! notes: RFC 8259 (the JSON the mapping names) defines the grammar. A comma or
//!   bracket inside a quoted JSON string is string data, not a separator, so
//!   the first and last inputs are well-formed. The second has no comma between
//!   its members; the third has a further value after the last member.
module Conf;

import System.Serialization;

record RS(V: string, W: bool): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(Try("{\"V\": \"a,b\", \"W\": true}"));
    Console.WriteLine(Try("{\"V\": \"a\" \"W\": true}"));
    Console.WriteLine(Try("{\"V\": \"a\", \"W\": true false}"));
    Console.WriteLine(Try("{\"V\": \"[1 2]\", \"W\": false}"));
  end Main;

  private static method Try(j: string): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j);
      var v := RS.Deserialize(d);
      r := "OK " + v.V + " " + v.W.ToString();
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end Try;
end Program;
