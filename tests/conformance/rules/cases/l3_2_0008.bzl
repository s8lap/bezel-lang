//! case: l3_2_0008
//! doc: stdlib
//! section: 3.2
//! title: Encoding.GetByteCount has no overload taking an int
//! rule: Stdlib §3.2 declares `GetByteCount(s: string)` and `GetByteCount(s: ReadOnlyStringSpan)` only; `int` converts to
//!   neither (§5.1), so `GetByteCount(5)` has no applicable overload.
//! cites: 3.2
//! expect: reject
//! diagnostic: expects a `string`
module Conf;

public class Program
  public static method Main()
    var x := Encoding.UTF8.GetByteCount(5);
    Console.WriteLine(x);
  end Main;
end Program;
