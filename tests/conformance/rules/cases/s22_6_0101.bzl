//! case: s22_6_0101
//! doc: spec
//! section: 22.6
//! title: a string argument to RawBytes.From, whose parameter is Array<byte>, has no applicable candidate
//! rule: If no candidate is applicable at all — no candidate is reachable: a
//!   conversion §5.1 does not admit as implicit, ... — the call is a compile
//!   error with no candidates cited, because no overload was reachable.
//! cites: 22.6, 5.1
//! expect: reject
//! diagnostic: RawBytes.From
module Conf;

public class Program
  public static method Main()
    using var r := RawBytes.From("hello");
    Console.WriteLine(r.Length);
  end Main;
end Program;
