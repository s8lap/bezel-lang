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
    var w: Widget? := null;
    Console.WriteLine(w!.Name);
  end Main;
end Program;
