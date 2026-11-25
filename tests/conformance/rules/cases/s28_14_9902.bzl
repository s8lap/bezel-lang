//! case: s28_14_9902
//! doc: spec
//! section: 28.14
//! title: an override storing a closure that captures its plain function parameter in a field is rejected
//! rule: captured by a closure that itself escapes the callee
//! cites: 28.14, 27.5
//! expect: reject
//! diagnostic: scoped
//! issue: 9150
//! notes: Harvested from the #9150 review (c_escaping_closure). The stored lambda captures the
//!   call-bounded parameter and escapes into a field, which is the transitive form of the field-store
//!   ban.
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
    this.saved := lambda() => q() + 1;
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
