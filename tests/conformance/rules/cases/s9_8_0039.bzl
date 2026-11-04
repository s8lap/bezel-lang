//! case: s9_8_0039
//! doc: spec
//! section: 9.8
//! title: a record's inline-array component is record-admissible and round-trips
//! rule: A record's serialized components must therefore be
//!   **record-admissible**: a primitive, `string`, an enum, … or a non-owning
//!   inline array `[N]T`/`[N]T?` of record-admissible element type —
//!   value-shaped storage throughout.
//! cites: 9.8, 15.1
//! expect: run
//! stdout: 1 2 3
//! notes: twin of s9_8_0038; observed by a JSON agent round trip through the
//!   synthesized members.
module Conf;

import System.Serialization;

public record Triple(V: [3]int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var p := new Triple([1, 2, 3]);
    using var s := new JsonSerializer();
    p.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    var back := Triple.Deserialize(d);
    Console.WriteLine($"{back.V[0]} {back.V[1]} {back.V[2]}");
  end Main;
end Program;
