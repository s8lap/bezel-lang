module Probe;
// DESTRUCTOR WINDOW, the external-source holder (flush-on-dispose): a holder whose destructor reads a source declared BEFORE it. Scope end disposes in reverse declaration order, so the holder's destructor reads a live source. EXPECT: clean in both modes. This is the idiom a blanket "no stored-borrow read in a destructor" rule would retire.
public class Sink
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public method Write(s: string) => Console.WriteLine(_name + " <- " + s);
  destructor
    Console.WriteLine("sink closed");
  end destructor;
end Sink;
public class Writer
  private _sink: Sink;
  public constructor(sink: Sink)
    _sink := sink;
  end constructor;
  destructor
    _sink.Write("flush");
  end destructor;
end Writer;
public class Program
  public static method Main(): int
    using var s := new Sink("out");
    using var w := new Writer(s);
    Console.WriteLine("made");
    return 0;
  end Main;
end Program;
