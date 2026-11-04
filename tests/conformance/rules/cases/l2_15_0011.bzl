//! case: l2_15_0011
//! doc: stdlib
//! section: 2.15
//! title: BinaryDeserializer's ignoreUnknownFields binds by name after a positional input
//! rule: public constructor(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool := false);
//! cites: 2.15, 22.5
//! expect: run
//! stdout: 7 9
module Conf;

import System.Serialization;

public class Program
  public static method Main()
    using var w := new BinarySerializer();
    w.WriteInt(7);
    w.WriteInt(9);
    using var bytes := w.ToBytes();
    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(), ignoreUnknownFields := true);
    var a := d.ReadInt();
    var b := d.ReadInt();
    Console.WriteLine($"{a} {b}");
  end Main;
end Program;
