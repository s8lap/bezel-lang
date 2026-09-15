//! case: l4_5_0002
//! doc: stdlib
//! section: 4.5
//! title: the OwningArray<T> indexer setter disposes the previous slot value
//! rule: The indexer getter produces a borrow of the slot's owned element;
//!   the setter takes ownership and disposes the previous value first —
//!   property [i: int]: T get; set(owned);
//! cites: 4.5, 28.7, 28.11
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
    pool[0] := new Widget();
    Console.WriteLine("after replace");
  end Main;
end Program;
