module Probe;
// DESTRUCTOR WINDOW, the external-source holder declared BEFORE its source (a later store): the source is disposed first at scope end and the holder's destructor reads it. EXPECT today: compiles (field-held trust); checked = DisposedAccessException. Under 0212: error at the store (the holder's release is a use, so the source must be declared before the holder).
public class Sink
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public method Write(s: string) => Console.WriteLine(_name + " <- " + s);
end Sink;
public class Writer
  private _sink: Sink? := null;
  public constructor()
  end constructor;
  public method Attach(sink: Sink) => _sink := sink;
  destructor
    var s := _sink;
    if s <> null then
      s.Write("flush");
    end;
  end destructor;
end Writer;
public class Program
  public static method Main(): int
    using var w := new Writer();
    using var s := new Sink("out");
    w.Attach(s);
    Console.WriteLine("made");
    return 0;
  end Main;
end Program;
