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
  public static async method Main(): Task
    using var t := Task.FromOwnedResult<Widget>(new Widget("a"));
    using var a := await t;
    Console.WriteLine(a.Name);
    using var b := await t;
    Console.WriteLine(b.Name);
  end Main;
end Program;
