//! case: s8_7_0002
//! doc: spec
//! section: 8.7
//! title: Dispose through an Object-typed receiver holding a value is rejected
//! rule: "`Dispose()` is class-only." (§8.7, v0.340) `Object` is not a class that generates `IDisposable`: the call `q.Dispose()` over `q: Object` (holding a boxed int) names no member `Dispose` of `Object`.
//! cites: 8.1
//! expect: reject
//! diagnostic: Dispose
module Conf;

public class Program
  public static method Main()
    var p: Object := 3;
    var q := p;
    q.Dispose();
  end Main;
end Program;
