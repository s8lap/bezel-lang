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

// K3.13 today: `var b := a;` is a borrow; a stays owned and usable; one disposal at scope end.
// In the flipped language (unmarked read consumes) the read of a after the binding is rejected.
public class Program
  public static method Main()
    using var a := new Widget("a");
    var b := a;
    a.Touch();
    b.Touch();
  end Main;
end Program;
