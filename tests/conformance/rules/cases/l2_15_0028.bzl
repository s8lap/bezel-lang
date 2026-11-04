//! case: l2_15_0028
//! doc: stdlib
//! section: 2.15
//! title: a JSON array's elements must be separated by a comma: an inline array
//!   component
//! rule: Malformed input, type mismatches (a `ReadInt` when the next token is a
//!   string), and depth-limit violations raise `SerializationException`
//! cites: 2.15
//! expect: run
//! output: OK 1 2
//!   OK 1 2
//!   OK 1 2
//!   SER
//! notes: RFC 8259 (the JSON the mapping names) defines the grammar. A JSON array
//!   separates its elements with ','. The input '[1 2]' has exactly the two
//!   elements the component holds, so its only fault is the missing comma.
module Conf;

import System.Serialization;

record RA(V: [2]int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(Try("{\"V\": [1, 2]}"));
    Console.WriteLine(Try("{\"V\":[1,2]}"));
    Console.WriteLine(Try("{\"V\": [ 1 , 2 ]}"));
    Console.WriteLine(Try("{\"V\": [1 2]}"));
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
