module Relax;

public class Widget
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  public property Name: string => _name;

  public method Touch()
    Console.WriteLine($"touch {_name}");
  end Touch;

  destructor
    Console.WriteLine($"dispose {_name}");
  end destructor;
end Widget;

// K3.13 today: a named owned binding feeding an `owned` parameter needs the marker
// (rejected without it). In the flipped language the bare `s.Adopt(w)` would be the move.
public class Sink
  public constructor()
  end constructor;

  public method Adopt(owned w: Widget)
    w.Touch();
  end Adopt;
end Sink;

public class Program
  public static method Main()
    var s := new Sink();
    var w := new Widget("w");
    s.Adopt(w);
  end Main;
end Program;
