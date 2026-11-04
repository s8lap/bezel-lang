//! case: s9_8_0022
//! doc: spec
//! section: 9.8
//! title: a serialized skip component round-trips as its default
//! rule: **`serialized skip`** — the component is excluded from both directions
//!   entirely. The synthesized `Serialize` does not write it; the synthesized
//!   `Deserialize` does not read it and supplies the component's `default(T)`
//!   (§17.11) to the constructor.
//! cites: 9.8, 17.11
//! expect: run
//! stdout: 1 0
module Conf;

import System.Serialization;

public record Pair(A: int, B: int serialized skip): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var p := new Pair(1, 7);
    using var s := new JsonSerializer();
    p.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    var back := Pair.Deserialize(d);
    Console.WriteLine($"{back.A} {back.B}");
  end Main;
end Program;
