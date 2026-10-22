//! case: l9_8_0006
//! doc: stdlib
//! section: 9.8
//! title: Range.Contains over a byte range, directly and through a lambda
//! rule: Stdlib §9.8 (v0.340): "`Contains(v)` is `true` iff the range is nonempty under iteration and `v` lies within
//!   the endpoint extent (exclusive upper bound excluded) ... `Step` affects membership only through
//!   direction/emptiness." For `Range<byte>` over `1..4`, 3 is a member and 9 is not, whether asked directly or
//!   through a capturing lambda.
//! cites: 16.3
//! expect: run
//! output: True
//!   False
//!   True
module Conf;

public class Program
  public static method Main()
    var lo: byte := 1;
    var hi: byte := 4;
    var d := lo..hi;
    var i := lambda(x: byte) => d.Contains(x);
    var z: byte := 3;
    var y: byte := 9;
    Console.WriteLine(i(z));
    Console.WriteLine(i(y));
    Console.WriteLine(d.Contains(z));
  end Main;
end Program;
