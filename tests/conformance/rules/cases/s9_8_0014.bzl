//! case: s9_8_0014
//! doc: spec
//! section: 9.8
//! title: a nested serializable record component is written by its own Serialize
//! rule: For each component in declaration order: `s.WriteFieldName("<name>")`
//!   followed by the component's encoding — … the nested value's
//!   `Serialize(s)` for a serializable record; …
//! cites: 9.8
//! expect: run
//! stdout: P(A=4) K=5
module Conf;

import System.Serialization;

public record Inner(A: int): ISerializable, IDeserializable;

public record Outer(P: Inner, K: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var o := new Outer(new Inner(4), 5);
    using var s := new JsonSerializer();
    o.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    _ := d.BeginObject();
    var n1 := d.ReadFieldName();
    _ := d.BeginObject();
    var n2 := d.ReadFieldName();
    var a := d.ReadInt();
    d.EndObject();
    var n3 := d.ReadFieldName();
    var k := d.ReadInt();
    d.EndObject();
    Console.WriteLine($"{n1}({n2}={a}) {n3}={k}");
  end Main;
end Program;
