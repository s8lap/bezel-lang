//! case: s28_3_8116
//! doc: spec
//! section: 28.3
//! title: a capture-free with-replacement of a body-record field declared Object is
//!  admitted
//! rule: At every set-erasing position a function value with a non-empty source set is
//!  rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: run
//! output: 1
//! notes: Admitting twin of s28_3_8114.
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
    var d := new R(1);
    var c := d with(Extra := lambda() => 3);
    Console.WriteLine(d.A);
    return 0;
  end Main;
end Program;
