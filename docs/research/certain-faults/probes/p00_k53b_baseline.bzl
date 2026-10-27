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
    var a := new Widget("a");
    var b := a;
    a.Dispose();
    Console.WriteLine(b.Name);
  end Main;
end Program;
