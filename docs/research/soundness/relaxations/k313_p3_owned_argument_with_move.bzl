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

// K3.13 today: the marked spelling. Expected trace: touch w, dispose w (inside Adopt, at
// the owned parameter's scope end), then "after".
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
    s.Adopt(move w);
    Console.WriteLine("after");
  end Main;
end Program;
