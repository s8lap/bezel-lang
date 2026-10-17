//! case: s3_2_0003
//! doc: spec
//! section: 3.2
//! title: a record this stored into an Array element reads back its field values
//! rule: A **value type** is copied on assignment; each variable holds an independent value.
//! cites: 3.2, 28.11
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
