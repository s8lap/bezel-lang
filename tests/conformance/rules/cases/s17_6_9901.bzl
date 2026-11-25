//! case: s17_6_9901
//! doc: spec
//! section: 17.6
//! title: decrementing OwningBuffer.Length is rejected
//! rule: Read-only locations cannot be incremented.
//! cites: 17.6, 4.14, 23.1
//! expect: reject
//! diagnostic: read-only
//! issue: 9250
//! notes: Harvested from the #9250 review (rej_ob_dec). OwningBuffer<T>.Length is `pure readonly` (stdlib
//!   4.14), so it is a read-only location and `--` is an increment form (17.6).
module Conf;

public class Program
  public static method Main()
    using var b := new OwningBuffer<byte>(4);
    b.Length--;
  end Main;
end Program;
