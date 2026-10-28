module Conf;
// Family A: an interface-typed plain parameter stored into an interface-typed plain field. EXPECT: report.
public interface ISink
  method Put(x: int);
end ISink;
public class Logger
  private _sink: ISink;
  public constructor(sink: ISink)
    _sink := sink;
  end constructor;
end Logger;
