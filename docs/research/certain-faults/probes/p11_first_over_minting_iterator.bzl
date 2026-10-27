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
  public static method Gen(n: int): IEnumerable<Widget>
    for var i in 0..<n do
      yield return new Widget($"w{i}");
    end;
  end Gen;
  public static method Main()
    using var g := Gen(3);
    var p := g.First();
    Console.WriteLine(p.Name);
  end Main;
end Program;
