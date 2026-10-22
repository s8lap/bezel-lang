//! case: l9_8_0009
//! doc: stdlib
//! section: 9.8
//! title: Range<int>.Contains rejects a uint argument
//! rule: Stdlib §9.8 (v0.340): "`Contains(v)` is `true` iff the range is nonempty under iteration and `v` lies within
//!   the endpoint extent (exclusive upper bound excluded) ... `Step` affects membership only through
//!   direction/emptiness." `Contains(v)` takes the range's element type `T`; §5.1 under "The following are **never implicit**" lists "Between signed and unsigned types of the same or narrower width (e.g. `int → uint`, `uint → int`"; so a `uint`
//!   argument to a `Range<int>` is rejected: a `uint` is not implicitly convertible to `int`.
//! cites: 5.1
//! expect: reject
//! diagnostic: implicitly convertible
module Conf;

public class Program
  public static method Main()
    var q := 1..3;
    var u: uint := 2u;
    var f := lambda() => q.Contains(u);
    Console.WriteLine(f());
  end Main;
end Program;
