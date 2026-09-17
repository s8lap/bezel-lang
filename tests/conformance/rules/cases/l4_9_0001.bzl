//! case: l4_9_0001
//! doc: stdlib
//! section: 4.9
//! title: OwningDictionary<K,V>.Add disposes the passed value when the key is a duplicate
//! rule: Add(key: K, owned value: V); // ArgumentException if the key is
//!   present; the passed value was consumed at the call site and is
//!   disposed on that path (v0.195).
//! cites: 4.9, 28.11
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
    using var d := new OwningDictionary<string, Widget>();
    d.Add("a", new Widget());
    try
      d.Add("a", new Widget());
    catch
      ArgumentException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
