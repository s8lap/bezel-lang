//! case: s9_8_0011
//! doc: spec
//! section: 9.8
//! title: a renamed component is written under its serialized name
//! rule: **`serialized "<name>"`** — the component is serialized and
//!   deserialized under `<name>` instead of its declared identifier.
//! cites: 9.8
//! expect: run
//! stdout: alpha=5 B=6
module Conf;

import System.Serialization;

public record Pair(A: int serialized "alpha", B: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var p := new Pair(5, 6);
    using var s := new JsonSerializer();
    p.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    _ := d.BeginObject();
    var n1 := d.ReadFieldName();
    var v1 := d.ReadInt();
    var n2 := d.ReadFieldName();
    var v2 := d.ReadInt();
    d.EndObject();
    Console.WriteLine($"{n1}={v1} {n2}={v2}");
  end Main;
end Program;
