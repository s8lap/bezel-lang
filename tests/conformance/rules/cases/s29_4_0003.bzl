//! case: s29_4_0003
//! doc: spec
//! section: 29.4
//! title: a borrow-bearing record local crossing a yield must have a frame-resident source
//! rule: A borrow-bearing record local (§9.7) whose live range crosses a
//!   `yield return` is subject to the same frame-resident-source rule as
//!   §30.1's across-suspension rule: every borrow the local carries must
//!   have a source the frame itself owns — an owned local, a `using var`, or
//!   an `owned` parameter of the iterator body.
//! cites: 29.4, 30.1
//! expect: reject
//! diagnostic: frame-resident
module Conf;

public class Program
  public static method Peek(arr: Array<int>): IEnumerable<int>
    var span := arr.AsSpan();
    yield return span.Length;
    yield return span.Length;
  end Peek;
end Program;
