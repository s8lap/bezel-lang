//! case: s9_7_0031
//! doc: spec
//! section: 9.7
//! title: a closure over a span later reassigned to a local array's span bounds the frame it rides
//! rule: the capture contributes the sources of the borrows the record carries to
//!   the closure's source set, every value the captured binding is assigned, in
//!   the enclosing body or in a lambda body, counted ... and the results of
//!   invoking it, and every deferred-activation frame or storage-bounded holder
//!   it rides, are bounded by them
//! cites: 9.7, 28.3
//! expect: reject
//! diagnostic: outlive
//! issue: 5718
module Conf;


public class Maker
  public method Window(a: Array<int>): IEnumerable<int>
    var s := a.AsSpan();
    var f := lambda(x: int) => s[0] + x;
    var zs: Array<int> := [9];
    s := zs.AsSpan();
    return a.Select(f);
  end Window;
end Maker;
