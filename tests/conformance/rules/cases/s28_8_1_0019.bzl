//! case: s28_8_1_0019
//! doc: spec
//! section: 28.8.1
//! title: the replacing move through a borrowed parameter receiver is admitted
//! rule: The receiver `x` in `move x.f` is any object expression ... whose
//!   field `f` is `owned` and accessible at the site ... a borrowed
//!   parameter included, as v0.332 ruled for the take
//! cites: 28.8.1, 13.1
//! expect: run
//! stdout: a b
//! issue: 5923
module Conf;

public class Res
  public readonly property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Res;

public class Holder
  private owned _r: Res := new Res("a");

  public static method Replace(h: Holder): owned Res
    return move h._r with new Res("b");
  end Replace;

  public property Current: string => _r.Name;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    using var t := Holder.Replace(h);
    Console.WriteLine($"{t.Name} {h.Current}");
  end Main;
end Program;
