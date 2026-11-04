//! case: s9_8_0021
//! doc: spec
//! section: 9.8
//! title: a missing serialized optional component reads as its default
//! rule: On read, a missing field yields `default(T)` instead of raising
//!   `SerializationException`; a present field is read normally.
//! cites: 9.8, 17.11
//! expect: run
//! output: 5 0
//!   5 6
module Conf;

import System.Serialization;

public record Pair(A: int, B: int serialized optional): ISerializable, IDeserializable;

public class Program
  public static method Main()
    using var d1 := new JsonDeserializer("{\"A\": 5}");
    var p := Pair.Deserialize(d1);
    Console.WriteLine($"{p.A} {p.B}");
    using var d2 := new JsonDeserializer("{\"A\": 5, \"B\": 6}");
    var q := Pair.Deserialize(d2);
    Console.WriteLine($"{q.A} {q.B}");
  end Main;
end Program;
