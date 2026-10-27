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

// K5.2 in a loop: the borrow taken before the loop is live around the back edge, so the
// reassignment in the body is frozen. Under the mutation, the second iteration's read
// through b faults.
public class Program
  public static method Main()
    var a := new Widget("w0");
    var b := a;
    var i := 0;
    while i < 2 do
      Console.WriteLine(b.Name);
      a := new Widget($"w{i + 1}");
      i := i + 1;
    end;
  end Main;
end Program;
