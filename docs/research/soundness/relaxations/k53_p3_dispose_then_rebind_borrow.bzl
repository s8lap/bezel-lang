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

// K5.3 and §28.2: straight-line use of the DISPOSED binding itself is a mandatory
// warning; a fresh borrow taken after the kill reads the flagged object and faults.
public class Program
  public static method Main()
    var a := new Widget("a");
    a.Dispose();
    var c := a;                 // borrow of an explicitly-disposed binding: warning (§28.2)
    Console.WriteLine(c.Name);  // faults in checked mode
  end Main;
end Program;
