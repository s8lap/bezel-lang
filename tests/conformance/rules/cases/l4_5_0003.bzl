//! case: l4_5_0003
//! doc: stdlib
//! section: 4.5
//! title: OwningArray<T>.RemoveAt disposes the removed element
//! rule: RemoveAt(index: int); // disposes the removed element
//! cites: 4.5, 28.11
//! expect: run
//! stdout: widget disposed
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    pool.RemoveAt(0);
    Console.WriteLine("after removeat");
  end Main;
end Program;
