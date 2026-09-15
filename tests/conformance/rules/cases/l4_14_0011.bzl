//! case: l4_14_0011
//! doc: stdlib
//! section: 4.14
//! title: ReadOnlySpan<T> has no ToArray() member
//! rule: ReadOnlySpan<T> declared ToArray(): owned Array<T> from v0.222 until
//!   v0.277, which deleted it; the compiler never admitted the call, and the
//!   absence is now recorded -- there is no materializer on this family.
//! cites: 4.14
//! expect: reject
//! diagnostic: ToArray
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<int>(3, lambda(i: int): int => i);
    var ro := buf.AsReadOnlySpan();
    using var copy := ro.ToArray();
  end Main;
end Program;
