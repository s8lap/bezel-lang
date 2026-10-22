//! case: l9_8_0008
//! doc: stdlib
//! section: 9.8
//! title: the bool result of Range.Contains does not convert implicitly to int
//! rule: Stdlib §9.8 (v0.340): "`Contains(v)` is `true` iff the range is nonempty under iteration and `v` lies within
//!   the endpoint extent (exclusive upper bound excluded) ... `Step` affects membership only through
//!   direction/emptiness." The result is a `bool`; "No implicit conversion to or from any other type" (§4.1 `bool`, v0.340), so
//!   `var n: int := q.Contains(2);` is a type error: a `bool` is not implicitly convertible to `int`.
//! cites: 5.1
//! expect: reject
//! diagnostic: implicitly convertible
module Conf;

public class Program
  public static method Main()
    var q := 1..3;
    var n: int := q.Contains(2);
    Console.WriteLine(n);
  end Main;
end Program;
