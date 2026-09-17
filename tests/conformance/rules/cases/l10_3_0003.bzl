//! case: l10_3_0003
//! doc: stdlib
//! section: 10.3
//! title: a CopyTo window outside the destination's Count raises IndexOutOfRangeException
//! rule: a window outside destination's Count raises
//!   IndexOutOfRangeException (the §1/§7.1 window rule).
//! cites: 10.3
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var src := new Array<byte>(3, 0);
    src[0] := 1;
    src[1] := 2;
    src[2] := 3;
    using var rb := RawBytes.From(src);
    var dest := new Array<byte>(2, 0);
    try
      rb.CopyTo(dest, 0);
    catch
      IndexOutOfRangeException => Console.WriteLine("caught"),
    end try;
  end Main;
end Program;
