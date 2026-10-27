module Probe;
public class Widget
  public property Name: string;
  public constructor(name: string)
    Name := name;
  end constructor;
  destructor
  end destructor;
end Widget;
public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget("a"));
    var w := pool[0];
    pool[0] := new Widget("b");
    Console.WriteLine(w.Name);
  end Main;
end Program;
