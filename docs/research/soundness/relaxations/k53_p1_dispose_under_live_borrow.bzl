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

// K5.3 today: an explicit Dispose() while a live borrow exists is ADMITTED (unfrozen);
// the later read through the borrow faults in checked mode (K4.1's backstop).
public class Program
  public static method Main()
    var a := new Widget("a");
    var b := a;                 // unmarked read: a borrow of a (K3.13)
    a.Dispose();                // site-announced kill (K3.9) under the live borrow b
    Console.WriteLine(b.Name);  // use through the surviving borrow
  end Main;
end Program;
