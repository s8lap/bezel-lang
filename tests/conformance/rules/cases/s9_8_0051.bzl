//! case: s9_8_0051
//! doc: spec
//! section: 9.8
//! title: a serialized rename on a choice case is admitted and names the discriminator
//! rule: The `serialized` clause on a **case** admits the rename form only (a
//!   case is not omittable — the enum-constant rule);
//! cites: 9.8, 35.1
//! expect: run
//! stdout: $case=on
//! notes: twin of s9_8_0046 (one legal edit: `serialized skip` becomes
//!   `serialized "on"`).
module Conf;

import System.Serialization;

public choice Mode: ISerializable, IDeserializable
  On serialized "on",
  Off;
end Mode;

public class Program
  public static method Main()
    var m := Mode.On;
    using var s := new JsonSerializer();
    m.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    _ := d.BeginObject();
    var n := d.ReadFieldName();
    var k := d.ReadString();
    d.EndObject();
    Console.WriteLine($"{n}={k}");
  end Main;
end Program;
