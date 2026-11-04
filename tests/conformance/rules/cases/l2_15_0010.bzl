//! case: l2_15_0010
//! doc: stdlib
//! section: 2.15
//! title: BinaryDeserializer reads through the ReadOnlyArraySpan constructor over an Array<byte>
//! rule: public constructor(input: ReadOnlyArraySpan<byte>, ignoreUnknownFields: bool :=
//!   false);
//! cites: 2.15, 4.4
//! expect: run
//! stdout: 7
module Conf;

import System.Serialization;

public class Program
  public static method Main()
    using var w := new BinarySerializer();
    w.WriteInt(7);
    using var bytes := w.ToBytes();
    using var arr := new Array<byte>();
    for var i in 0..<bytes.Length do
      arr.Add(bytes[i]);
    end;
    using var d := new BinaryDeserializer(arr.AsReadOnlySpan());
    Console.WriteLine(d.ReadInt());
  end Main;
end Program;
