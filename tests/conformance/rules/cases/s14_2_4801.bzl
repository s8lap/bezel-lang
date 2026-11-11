//! case: s14_2_4801
//! doc: spec
//! section: 14.2
//! title: Array<Object>.Sort() is rejected: Object declares no IComparable conformance
//! rule: `Object` is a compiler intrinsic; method bodies are provided by the runtime. The declaration above defines the public surface.
//! cites: 8.1, stdlib 2.8, 14.2
//! expect: reject
//! diagnostic: requires T: IComparable<T>
module Conf;

public class Program
  public static method Main()
    var a: Array<Object> := [];
    a.Sort();
  end Main;
end Program;
