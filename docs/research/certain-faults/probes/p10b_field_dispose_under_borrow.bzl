module Probe;
public class Widget
  public property Name: string;
  public constructor(name: string)
    Name := name;
  end constructor;
  destructor
  end destructor;
end Widget;
public class Holder
  private owned _w: Widget := new Widget("first");
  public method Swap()
    var b := _w;
    _w.Dispose();
    Console.WriteLine(b.Name);
  end Swap;
end Holder;
public class Program
  public static method Main()
    using var h := new Holder();
    h.Swap();
  end Main;
end Program;
