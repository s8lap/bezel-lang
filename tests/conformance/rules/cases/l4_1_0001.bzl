//! case: l4_1_0001
//! doc: stdlib
//! section: 4.1
//! title: Array<T>.Dispose() does not dispose its stored elements
//! rule: Element disposal is never performed by or through a borrowing
//!   collection: a holder that owns its elements uses the owning family,
//!   whose Dispose() cascades; the borrowing family's Dispose() is a no-op.
//! cites: 28.6, 28.11
//! expect: run
//! stdout: still alive
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
    using var w := new Widget();
    do
      using var arr := new Array<Widget>();
      arr.Add(w);
    end;
    Console.WriteLine("still alive");
  end Main;
end Program;
