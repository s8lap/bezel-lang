//! case: l2_15_0031
//! doc: stdlib
//! section: 2.15
//! title: a JSON array's elements must be separated by a comma: an inline array of
//!   records
//! rule: Malformed input, type mismatches (a `ReadInt` when the next token is a
//!   string), and depth-limit violations raise `SerializationException`
//! cites: 2.15
//! expect: run
//! output: OK 1 2 3 4
//!   OK 1 2 3 4
//!   SER
//!   SER
//! notes: RFC 8259 (the JSON the mapping names) defines the grammar. The third input
//!   has no comma between the two elements; the fourth has none between the
//!   members of the first element.
module Conf;

import System.Serialization;

record RT(V: int, W: int): ISerializable, IDeserializable;
record RAR(V: [2]RT): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(Try("{\"V\": [{\"V\": 1, \"W\": 2}, {\"V\": 3, \"W\": 4}]}"));
    Console.WriteLine(Try("{\"V\":[{\"V\":1,\"W\":2},{\"V\":3,\"W\":4}]}"));
    Console.WriteLine(Try("{\"V\": [{\"V\": 1, \"W\": 2} {\"V\": 3, \"W\": 4}]}"));
    Console.WriteLine(Try("{\"V\": [{\"V\": 1 \"W\": 2}, {\"V\": 3, \"W\": 4}]}"));
  end Main;

  private static method Try(j: string): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j);
      var v := RAR.Deserialize(d);
      r := $"OK {v.V[0].V} {v.V[0].W} {v.V[1].V} {v.V[1].W}";
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end Try;
end Program;
