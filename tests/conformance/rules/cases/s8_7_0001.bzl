//! case: s8_7_0001
//! doc: spec
//! section: 8.7
//! title: Dispose through a nullable Object-typed receiver is rejected
//! rule: "`Dispose()` is class-only." (§8.7, v0.340) `Object` is not a class that generates `IDisposable`: the statement `o?.Dispose()` over `o: Object?` names no member `Dispose` of `Object`.
//! cites: 8.1
//! expect: reject
//! diagnostic: Dispose
module Conf;

public class Program
  public static method Main()
    var o: Object? := null;
    o?.Dispose();
  end Main;
end Program;
