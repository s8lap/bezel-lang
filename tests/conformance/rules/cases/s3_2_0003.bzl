//! case: s3_2_0003
//! doc: spec
//! section: 3.2
//! title: a record this appended to an Array with Add reads back its field values
//! rule: A **value type** is copied on assignment
//! cites: 3.2, 28.11
//! issue: 6317
//! notes: the Array Add sink is the one #6317 patched (its body names
//!   `a.Add(this)`); #6320 is the residual for the other sinks. The program
//!   shows `this` reaching the array as a value -- the record is never
//!   mutated, so it does not additionally show copy independence.
//! expect: run
//! stdout: 3
module Conf;

public record Pt(X: int)
  public method Keep(a: Array<Pt>) a.Add(this); end Keep;
end Pt;

public class M
  public static method Main(): int
    var a := new Array<Pt>();
    var p := new Pt(3);
    p.Keep(a);
    Console.WriteLine(a[0].X);
    return 0;
  end Main;
end M;
