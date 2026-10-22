//! case: s21_10_0016
//! doc: spec
//! section: 21.10
//! title: a local record is not visible outside the body that declares it
//! rule: "A `record` or `enum` may be declared as a **statement**, scoped to the enclosing body like a local variable"
//! cites: 9
//! expect: reject
//! diagnostic: local type
module Conf;

public class Program
  public static method Make(): int
    record Pt(x: int);
    var p := new Pt(1);
    return p.x;
  end Make;

  public static method Main()
    var q := new Pt(2);
    Console.WriteLine(q.x);
  end Main;
end Program;
