//! case: s3_2_0004
//! doc: spec
//! section: 3.2
//! title: a record this passed to an indexer set reads back its field values
//! rule: A **value type** is copied on assignment
//! cites: 3.2, 28.11
//! issue: 6320
//! notes: the indexer-set sink is the one #6320 patched (`a[i] := this`
//!   gave silent garbage); #6317 covered Array Add, which s3_2_0003 pins.
//!   The program shows `this` reaching the slot as a value -- the record is
//!   never mutated, so it does not additionally show copy independence.
//! expect: run
//! stdout: 4
module Conf;

public record Pt(X: int)
  public method Store(a: Array<Pt>) a[0] := this; end Store;
end Pt;

public class M
  public static method Main(): int
    var a := new Array<Pt>();
    a.Add(new Pt(1));
    var p := new Pt(4);
    p.Store(a);
    Console.WriteLine(a[0].X);
    return 0;
  end Main;
end M;
