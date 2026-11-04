//! case: s9_8_0015
//! doc: spec
//! section: 9.8
//! title: an enum component is written as its serialized-name string
//! rule: For each component in declaration order: `s.WriteFieldName("<name>")`
//!   followed by the component's encoding — … the enum-name string for an
//!   enum; …
//! cites: 9.8, 10.1
//! expect: run
//! stdout: L=High
//! notes: Level declares no conformance of its own — enums need none to be a
//!   component ("Enums need no conformance of their own").
module Conf;

import System.Serialization;

public enum Level
  Low, High;
end Level;

public record Setting(L: Level): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var v := new Setting(Level.High);
    using var s := new JsonSerializer();
    v.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    _ := d.BeginObject();
    var n := d.ReadFieldName();
    var name := d.ReadString();
    d.EndObject();
    Console.WriteLine($"{n}={name}");
  end Main;
end Program;
