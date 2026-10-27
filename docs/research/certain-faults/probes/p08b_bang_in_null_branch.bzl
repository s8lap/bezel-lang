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
  public static method Find(): owned Widget? => null;
  public static method Main()
    var w := Find();
    if w = null then
      Console.WriteLine(w!.Name);
    end;
  end Main;
end Program;
