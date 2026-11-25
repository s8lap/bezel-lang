//! case: s28_14_9901
//! doc: spec
//! section: 28.14
//! title: an override of an abstract member storing its plain function parameter in a field is rejected
//! rule: It may not be stored in a field of any object
//! cites: 28.14, 27.5
//! expect: reject
//! diagnostic: scoped
//! issue: 9150
//! notes: Harvested from the #9150 review (a_store_field). The abstract member's parameter is plain, so
//!   call-bounded, and the override's body stores it in a field. Deleting that one statement leaves a
//!   program that compiles.
module Conf;

public class Res
  public V: int;

  public constructor()
    V := 42;
  end constructor;
end Res;

public abstract class Fl
  public abstract method M(q: () -> int): int;

  public constructor()
  end constructor;
end Fl;

public class Sub: Fl
  private saved: (() -> int)?;

  public constructor()
  end constructor;

  public override method M(q: () -> int): int
    this.saved := q;
    return q();
  end M;

  public method Again(): int => this.saved!();
end Sub;

public class Program
  public static method Main()
    var t := 0;
    var s := new Sub();
    var fl: Fl := s;
    for var j in 0 ..< 100 do
      var r := new Res();
      var g := lambda() => r.V;
      t := t + fl.M(g);
    end for;
    t := t + s.Again();
    Console.WriteLine(t);
  end Main;
end Program;
