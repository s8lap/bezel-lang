//! case: s9_8_0017
//! doc: spec
//! section: 9.8
//! title: a duplicate-valued enum constant writes the first-declared name and reads every name
//! rule: **Duplicate constant values** (§10.2): the synthesized form writes the
//!   **first-declared** constant's name for that value and accepts every
//!   declared name on read (all denote the same value, so the round trip is
//!   stable);
//! cites: 9.8, 10.2
//! expect: run
//! output: Legacy
//!   1
//!   1
module Conf;

import System.Serialization;

public enum Code: ISerializable, IDeserializable
  Legacy := 1, Current := 1, Next := 2;
end Code;

public class Program
  public static method Main()
    using var s := new JsonSerializer();
    Code.Current.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    Console.WriteLine(d.ReadString());
    using var d1 := new JsonDeserializer("\"Current\"");
    Console.WriteLine(int(Code.Deserialize(d1)));
    using var d2 := new JsonDeserializer("\"Legacy\"");
    Console.WriteLine(int(Code.Deserialize(d2)));
  end Main;
end Program;
