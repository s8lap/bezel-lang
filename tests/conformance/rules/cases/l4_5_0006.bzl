//! case: l4_5_0006
//! doc: stdlib
//! section: 4.5
//! title: PopFirst raises when the array is empty
//! rule: PopFirst(): owned T; // = PopAt(0); raises if empty.
//! cites: 4.5
//! expect: run
//! stdout: caught
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    try
      using var w := pool.PopFirst();
    catch
      Exception => Console.WriteLine("caught")
    end;
  end Main;
end Program;
