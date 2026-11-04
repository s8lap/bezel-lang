//! case: l2_15_0036
//! doc: stdlib
//! section: 2.15
//! title: Indented does not affect a JSON read: a nested record round-trips compact
//!   and indented
//! rule: Indented: bool, // pretty-print (serializer); ignored on read
//! cites: 2.15, 9.8
//! expect: run
//! output: compact 1 2 3 4 5 6 x, [y] {z}: w
//!   indented 1 2 3 4 5 6 x, [y] {z}: w
//! notes: The record nests a record, an inline array of records that each hold an
//!   inline array, and a string whose characters include the JSON punctuation
//!   ',', '[', ']', '{', '}' and ':'. A read of the text the serializer wrote
//!   must give back the values written, whatever the Indented setting.
module Conf;

import System.Serialization;

record P2(V: int, W: int): ISerializable, IDeserializable;
record A2(V: [2]int): ISerializable, IDeserializable;
record Top(A: P2, B: [2]A2, S: string): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(RoundTrip("compact", false));
    Console.WriteLine(RoundTrip("indented", true));
  end Main;

  private static method RoundTrip(label: string, indented: bool): string
    var a1 := new A2([3, 4]);
    var a2 := new A2([5, 6]);
    var t := new Top(new P2(1, 2), [a1, a2], "x, [y] {z}: w");
    using var s := new JsonSerializer(new JsonOptions(indented, 2, false, false, 64));
    t.Serialize(s);
    var wire := s.ToString();
    var r: string := "";
    try
      using var d := new JsonDeserializer(wire);
      var b := Top.Deserialize(d);
      r := $"{label} {b.A.V} {b.A.W} {b.B[0].V[0]} {b.B[0].V[1]} {b.B[1].V[0]} {b.B[1].V[1]} {b.S}";
    catch
      SerializationException => r := label + " SER";
    end;
    return r;
  end RoundTrip;
end Program;
