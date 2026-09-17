//! case: l10_3_0002
//! doc: stdlib
//! section: 10.3
//! title: CopyTo writes Length bytes starting at destOffset
//! rule: CopyTo(destination, destOffset) // copies Length bytes into
//!   destination[destOffset .. destOffset+Length).
//! cites: 10.3
//! expect: run
//! output: 0
//!   0
//!   1
//!   2
//!   3
module Conf;

public class Program
  public static method Main()
    var src := new Array<byte>(3, 0);
    src[0] := 1;
    src[1] := 2;
    src[2] := 3;
    using var rb := RawBytes.From(src);
    var dest := new Array<byte>(5, 0);
    rb.CopyTo(dest, 2);
    Console.WriteLine(dest[0]);
    Console.WriteLine(dest[1]);
    Console.WriteLine(dest[2]);
    Console.WriteLine(dest[3]);
    Console.WriteLine(dest[4]);
  end Main;
end Program;
