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
  public static method Flag(): bool => true;
  public static method Main()
    var a := new Widget("a");
    if Flag() then a.Dispose(); end;
    Console.WriteLine(a.Name);
  end Main;
end Program;
