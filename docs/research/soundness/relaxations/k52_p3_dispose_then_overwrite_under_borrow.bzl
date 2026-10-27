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

// The keyword-visible spelling of k52_p1: dispose first (K5.3, admitted), then reassign the
// explicitly-disposed binding (state-match rule 3). Same run-time outcome as the mutated
// k52_p1 — b faults — but spelled so the reader sees the kill. This is what K5.2 buys.
public class Program
  public static method Main()
    var a := new Widget("first");
    var b := a;
    a.Dispose();                // the kill is announced here
    a := new Widget("second");  // reassigning an explicitly-disposed LHS: nothing displaced
    Console.WriteLine(b.Name);  // faults in checked mode
  end Main;
end Program;
