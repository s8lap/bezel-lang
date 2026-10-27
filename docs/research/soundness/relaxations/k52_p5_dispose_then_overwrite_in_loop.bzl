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

// The keyword-visible spelling of k52_p4: announce the kill, then reassign, inside the loop,
// with the pre-loop borrow b live around the back edge. Admitted today; the second
// iteration's read through b faults in checked mode. This is the run-time behaviour a
// relaxed K5.2 would give k52_p4 as written.
public class Program
  public static method Main()
    var a := new Widget("w0");
    var b := a;
    var i := 0;
    while i < 2 do
      Console.WriteLine(b.Name);
      a.Dispose();
      a := new Widget($"w{i + 1}");
      i := i + 1;
    end;
  end Main;
end Program;
