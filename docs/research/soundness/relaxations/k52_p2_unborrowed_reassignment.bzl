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

// K5.2 does not touch this: unborrowed reassignment stays legal (§28.3.9).
// Expected trace: dispose first (at the overwrite), touch second, dispose second (scope end).
public class Program
  public static method Main()
    var a := new Widget("first");
    a.Touch();
    a := new Widget("second");
    a.Touch();
  end Main;
end Program;
