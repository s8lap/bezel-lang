//! case: s28_7_0009
//! doc: spec
//! section: 28.7
//! title: a throwing RHS leaves the field untouched and still owned
//! rule: A throwing RHS leaves the field/local untouched and still-owned —
//!   the implicit path has no exception-unsafe window.
//! cites: 28.7
//! expect: run
//! output: caught
//!   done
//!   dispose first
module Conf;

public class Widget
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine($"dispose {_name}");
  end destructor;
end Widget;

public class RiskyFactory
  public static method Make(): owned Widget
    throw new InvalidOperationException("boom");
  end Make;
end RiskyFactory;

public class Holder
  private owned _w: Widget;

  public constructor()
    _w := new Widget("first");
  end constructor;

  public method TryReplace()
    try
      _w := RiskyFactory.Make();
    catch
      var ex: InvalidOperationException => Console.WriteLine("caught");
    end;
  end TryReplace;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    h.TryReplace();
    Console.WriteLine("done");
  end Main;
end Program;
