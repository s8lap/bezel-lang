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
    var w := new Widget("a");
    var o := new Widget("b");
    var pick := w ?? o;
    Console.WriteLine(pick.Name);
  end Main;
end Program;
