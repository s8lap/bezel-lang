//! case: s28_3_8114
//! doc: spec
//! section: 28.3
//! title: a with-replacement of a body-record field declared Object by a capturing lambda
//!  is rejected
//! rule: At every set-erasing position a function value with a non-empty source set is
//!  rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #8962. A body field declared Object is a position declared Object; the
//!  with-replacement stores the capturing lambda into it. Twin: s28_3_8116 (capture-free
//!  with-replacement admitted).
module Conf;

public class Res
  public V: int;
  public constructor() V := 42; end constructor;
end Res;
public record R(A: int)
  public Extra: Object := 5;
end R;
public class Program
  public static method Main(): int
    var r := new Res();
    var d := new R(1);
    var c := d with(Extra := lambda() => r.V);
    Console.WriteLine(d.A);
    return 0;
  end Main;
end Program;
