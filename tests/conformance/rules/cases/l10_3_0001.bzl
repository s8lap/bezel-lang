//! case: l10_3_0001
//! doc: stdlib
//! section: 10.3
//! title: RawBytes.From copies the source buffer; later mutation does not affect it
//! rule: RawBytes.From(buffer) allocates a fresh, owned RawBytes containing
//!   a copy of the source bytes. Mutations to the source after
//!   construction do not affect the RawBytes; the buffer is independent.
//! cites: 10.3
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    var src := new Array<byte>(3, 0);
    src[0] := 1;
    src[1] := 2;
    src[2] := 3;
    using var rb := RawBytes.From(src);
    src[0] := 99;
    var dest := new Array<byte>(3, 0);
    rb.CopyTo(dest, 0);
    Console.WriteLine(dest[0]);
  end Main;
end Program;
