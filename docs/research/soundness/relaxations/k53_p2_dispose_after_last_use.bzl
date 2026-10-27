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

// K5.3: the same shape with the borrow dead at the kill — admitted today and under
// every variant of the rule (b has no later use, so it is not live at the Dispose()).
public class Program
  public static method Main()
    var a := new Widget("a");
    var b := a;
    b.Touch();
    a.Dispose();
    Console.WriteLine("done");
  end Main;
end Program;
