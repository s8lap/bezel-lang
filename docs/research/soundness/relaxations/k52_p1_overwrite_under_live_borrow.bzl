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

// K5.2 today: drop-on-overwrite of a binding while a live borrow exists is REJECTED
// (the freeze; §28.7 "Overwrite while borrowed"). Under the mutation it is admitted and
// the read through b faults in checked mode, exactly like k53_p1.
public class Program
  public static method Main()
    var a := new Widget("first");
    var b := a;                 // borrow of a's current value
    a := new Widget("second");  // drop-on-overwrite disposes "first" silently (K3.11)
    Console.WriteLine(b.Name);  // b survives the kill
  end Main;
end Program;
