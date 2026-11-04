//! case: s9_8_0016
//! doc: spec
//! section: 9.8
//! title: an enum's synthesized Serialize is one WriteString of its serialized name, no object
//! rule: For an eligible enum, the synthesized `Serialize` is
//!   `s.WriteString("<member-name>")` (no surrounding object) — no failing case
//!   exists (§5.4).
//! cites: 9.8, 10.1
//! expect: run
//! output: lo
//!   High
//! notes: the first line also pins "its `serialized "<name>"` rename where one
//!   is declared, its identifier otherwise".
module Conf;

import System.Serialization;

public enum Level: ISerializable, IDeserializable
  Low serialized "lo", High;
end Level;

public class Program
  public static method Main()
    Show(Level.Low);
    Show(Level.High);
  end Main;

  private static method Show(v: Level)
    using var s := new JsonSerializer();
    v.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    Console.WriteLine(d.ReadString());
  end Show;
end Program;
