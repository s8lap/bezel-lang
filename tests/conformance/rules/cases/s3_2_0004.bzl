//! case: s3_2_0004
//! doc: spec
//! section: 3.2
//! title: a record this stored through an indexer set is an independent copy
//! rule: A **value type** is copied on assignment; each variable holds an independent value.
//! cites: 3.2, 28.11
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
