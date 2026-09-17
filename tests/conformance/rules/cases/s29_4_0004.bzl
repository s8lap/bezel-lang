//! case: s29_4_0004
//! doc: spec
//! section: 29.4
//! title: a borrow-bearing record local sourced from an owned iterator parameter is admitted across a yield
//! rule: every borrow the local carries must have a source the frame itself
//!   owns — an owned local, a `using var`, or an `owned` parameter of the
//!   iterator body ([§29.8](#sec-iterators-with-owned-parameters)).
//! cites: 29.4, 29.8
//! expect: accept
module Conf;

public class Program
  public static method Peek(owned arr: Array<int>): IEnumerable<int>
    var span := arr.AsSpan();
    yield return span.Length;
    yield return span.Length;
  end Peek;
end Program;
