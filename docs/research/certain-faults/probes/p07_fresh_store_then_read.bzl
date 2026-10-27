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
    var ws := new Array<Widget>();
    ws.Add(new Widget("a"));
    Console.WriteLine(ws[0].Name);
  end Main;
end Program;
