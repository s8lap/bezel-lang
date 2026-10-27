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

// K3.13 today: passing to a PLAIN parameter is a borrow, no marker, the caller keeps
// ownership. In the flipped language every such pass would need a borrow marker instead,
// or it would move w into Log and leave the caller's w unreadable.
public class Program
  public static method Log(w: Widget)
    Console.WriteLine($"log {w.Name}");
  end Log;

  public static method Main()
    using var w := new Widget("w");
    Log(w);
    Log(w);
    w.Touch();
  end Main;
end Program;
