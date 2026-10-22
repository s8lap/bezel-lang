//! case: l3_2_0010
//! doc: stdlib
//! section: 3.2
//! title: the span-source encode forms agree on a window of a string
//! rule: Stdlib §3.2: the span-source encode overloads have "identical contract to the string forms over the span's
//!   code-point range (`GetByteCount` and `GetBytes` agree ...)". The window `(1, 2)` of `"aé€z"` is `é€`, U+00E9 as
//!   C3 A9 and U+20AC as E2 82 AC: `GetBytes(s, destination)` returns 5, the allocating `GetBytes(s)` holds 5 bytes,
//!   `GetByteCount` is 5, and the destination begins C3 A9 E2.
//! cites: 3.2
//! expect: run
//! output: 5
//!   5
//!   5
//!   195
//!   169
//!   226
module Conf;

public class Program
  public static method Main()
    var rs := new ReadOnlyStringSpan("aé€z", 1, 2);
    using var dst := new Array<byte>(8, 0);
    var n := Encoding.UTF8.GetBytes(rs, dst.AsSpan());
    using var b := Encoding.UTF8.GetBytes(rs);
    Console.WriteLine(n);
    Console.WriteLine(b.Count);
    Console.WriteLine(Encoding.UTF8.GetByteCount(rs));
    Console.WriteLine(dst[0]);
    Console.WriteLine(dst[1]);
    Console.WriteLine(dst[2]);
  end Main;
end Program;
