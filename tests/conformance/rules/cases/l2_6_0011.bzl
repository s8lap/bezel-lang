//! case: l2_6_0011
//! doc: stdlib
//! section: 2.6
//! title: Guid.FromByteArray raises ArgumentException for 15 and 17 bytes and reads 16 zero bytes as Empty
//! rule: borrows; raises ArgumentException unless Count = 16
//! cites: 2.6
//! expect: run
//! output: ArgumentException
//!   ArgumentException
//!   True
//!   16
module Conf;

public class Program
  public static method Main()
    using var b15 := new Array<byte>(15, 0);
    using var b17 := new Array<byte>(17, 0);
    using var b16 := new Array<byte>(16, 0);
    try
      _ := Guid.FromByteArray(b15);
      Console.WriteLine("no throw 15");
    catch ArgumentException => Console.WriteLine("ArgumentException");
    end;
    try
      _ := Guid.FromByteArray(b17);
      Console.WriteLine("no throw 17");
    catch ArgumentException => Console.WriteLine("ArgumentException");
    end;
    Console.WriteLine(Guid.FromByteArray(b16).Equals(Guid.Empty));
    var g := Guid.Parse("00010203-0405-0607-0809-0a0b0c0d0e0f");
    using var gb := g.ToByteArray();
    Console.WriteLine(gb.Count);
  end Main;
end Program;
