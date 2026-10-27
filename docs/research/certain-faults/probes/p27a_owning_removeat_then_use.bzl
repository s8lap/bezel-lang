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
    pool.Add(new Widget("b"));
    var w := pool[0];
    pool.RemoveAt(0);
    Console.WriteLine(w.Name);
  end Main;
end Program;
