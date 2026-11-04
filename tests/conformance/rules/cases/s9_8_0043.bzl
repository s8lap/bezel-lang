//! case: s9_8_0043
//! doc: spec
//! section: 9.8
//! title: a choice is written as an object led by its $case discriminator
//! rule: **Wire shape.** The synthesized `Serialize` writes an object whose
//!   distinguished leading field is the **case discriminator** `"$case"` — the
//!   case's declared name, or its `serialized "<name>"` rename — followed by
//!   the active case's payload fields by name, in declaration order, each
//!   encoded per its component rule; a payload-free case writes the object
//!   with only the discriminator.
//! cites: 9.8, 35.13
//! expect: run
//! output: $case=Rect w=2 h=3
//!   $case=none
//! notes: observed by reading the JSON agent output back token by token; the
//!   discriminator's value is read as a string, its JSON form a case name.
module Conf;

import System.Serialization;

public choice Shape: ISerializable, IDeserializable
  Rect(w: int, h: int),
  Empty serialized "none";
end Shape;

public class Program
  public static method Main()
    var a := Shape.Rect(2, 3);
    using var s1 := new JsonSerializer();
    a.Serialize(s1);
    using var d1 := new JsonDeserializer(s1.ToString());
    _ := d1.BeginObject();
    var c1 := d1.ReadFieldName();
    var k1 := d1.ReadString();
    var n2 := d1.ReadFieldName();
    var w := d1.ReadInt();
    var n3 := d1.ReadFieldName();
    var h := d1.ReadInt();
    d1.EndObject();
    Console.WriteLine($"{c1}={k1} {n2}={w} {n3}={h}");
    var b := Shape.Empty;
    using var s2 := new JsonSerializer();
    b.Serialize(s2);
    using var d2 := new JsonDeserializer(s2.ToString());
    _ := d2.BeginObject();
    var c2 := d2.ReadFieldName();
    var k2 := d2.ReadString();
    d2.EndObject();
    Console.WriteLine($"{c2}={k2}");
  end Main;
end Program;
