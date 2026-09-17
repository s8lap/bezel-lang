//! case: l4_5_0011
//! doc: stdlib
//! section: 4.5
//! title: OwningArray<T>.AddRange drains its owned source argument
//! rule: AddRange(owned items: OwningArray<T>); // drains items into self.
//! cites: 4.5, 28.11
//! expect: run
//! stdout: 1
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var dst := new OwningArray<Widget>();
    var src := new OwningArray<Widget>();
    src.Add(new Widget());
    dst.AddRange(move src);
    Console.WriteLine(dst.Count);
  end Main;
end Program;
