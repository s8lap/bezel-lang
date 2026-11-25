//! case: s28_14_9903
//! doc: spec
//! section: 28.14
//! title: a capturing closure local passed twice to an interface member runs in a 200x loop
//! rule: The caller side is correspondingly unrestricted: any closure — capture-bearing or not — may be
//!   passed to a call-bounded parameter
//! cites: 28.14, 28.9, 27.5
//! expect: run
//! stdout: 19780
//! issue: 9150
//! notes: Harvested from the #9150 review (e_iface_string). The closure captures an owned object and a
//!   string and goes to a plain (call-bounded) parameter of an interface member twice per pass. Per
//!   call: 42 + len(`hello` + j) = 47 + digits(j); digits over 0..199 total 490. 2 * (200 * 47 + 490)
//!   = 19780.
module Conf;

public class Res
  public V: int;

  public constructor()
    V := 42;
  end constructor;
end Res;

public interface IFl
  method M(q: () -> int): int;
end IFl;

public class Sub: IFl
  public constructor()
  end constructor;

  public method M(q: () -> int): int => q();
end Sub;

public class Program
  public static method Main()
    var t := 0;
    var fl: IFl := new Sub();
    for var j in 0 ..< 200 do
      var r := new Res();
      var s := "hello" + j.ToString();
      var g := lambda() => r.V + s.Length;
      t := t + fl.M(g) + fl.M(g);
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
