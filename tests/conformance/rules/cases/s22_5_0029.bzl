//! case: s22_5_0029
//! doc: spec
//! section: 22.5
//! title: Span records and BinaryDeserializer bind positional, named and reordered arguments alike
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as
//!   written, for positional and named arguments alike; naming an argument changes which parameter it
//!   binds to, never when it runs. Each of the spellings below binds
//!   the same parameters, so every one reads the same elements (20, 30) or the same integer (42).
//! cites: 22.5, 22.6
//! expect: run
//! output: 2 20 30 20 2 20 30
//!   45000
//!   21000
module Conf;

import System.Serialization;

public class Program
  public static method Main()
    var tot := 0;
    for var i in 0..<100 do
      using var arr := new Array<int>();
      arr.Add(10);
      arr.Add(20);
      arr.Add(30);
      arr.Add(40);
      using var buf := new OwningBuffer<int>(4, lambda(k) => (k + 1) * 10);
      var a1 := new ArraySpan<int>(arr, 1, 2);
      var a2 := new ArraySpan<int>(Length := 2, Start := 1, Source := arr);
      var a3 := new ArraySpan<int>(arr, Length := 2, Start := 1);
      var a4 := new ReadOnlyArraySpan<int>(Start := 1, Length := 2, Source := arr);
      var a5 := new ReadOnlyArraySpan<int>(arr, 1, 2);
      var s1 := new Span<int>(buf, 1, 2);
      var s2 := new Span<int>(Length := 2, Buffer := buf, Start := 1);
      var s3 := new ReadOnlySpan<int>(Length := 2, Start := 1, Buffer := buf);
      var s4 := new ReadOnlySpan<int>(buf, 1, 2);
      tot := tot + a1[0] + a1[1] + a2[0] + a2[1] + a3[0] + a3[1] + a4[0] + a4[1] + a5[0] + a5[1]
                 + s1[0] + s1[1] + s2[0] + s2[1] + s3[0] + s3[1] + s4[0] + s4[1];
      if i = 0 then
        Console.WriteLine($"{a2.Length} {a2[0]} {a3[1]} {a4[0]} {s2.Length} {s2[0]} {s3[1]}");
      end;
    end;
    Console.WriteLine(tot);
    var tot2 := 0L;
    using var w := new BinarySerializer();
    w.WriteInt(42);
    using var bytes := w.ToBytes();
    for var i in 0..<100 do
      using var d1 := new BinaryDeserializer(bytes.AsReadOnlySpan());
      using var d2 := new BinaryDeserializer(bytes.AsReadOnlySpan(), true);
      using var d3 := new BinaryDeserializer(ignoreUnknownFields := true, input := bytes.AsReadOnlySpan());
      using var d4 := new BinaryDeserializer(input := bytes.AsReadOnlySpan());
      using var d5 := new BinaryDeserializer(bytes.AsReadOnlySpan(), ignoreUnknownFields := false);
      tot2 := tot2 + d1.ReadInt() + d2.ReadInt() + d3.ReadInt() + d4.ReadInt() + d5.ReadInt();
    end;
    Console.WriteLine(tot2);
  end Main;
end Program;
