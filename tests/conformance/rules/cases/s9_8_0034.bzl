//! case: s9_8_0034
//! doc: spec
//! section: 9.8
//! title: serialized names differing only in case are distinct
//! rule: Names are compared **ordinally** — the wire's own comparison, not the
//!   case-insensitive comparison this section's constructor matching uses.
//! cites: 9.8
//! expect: run
//! stdout: x=1 X=2
//! notes: an accept-and-observe case: under a case-insensitive comparison the
//!   two renames would collide (s9_8_0007's error).
module Conf;

import System.Serialization;

public record Pair(A: int serialized "x", B: int serialized "X"): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var p := new Pair(1, 2);
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
