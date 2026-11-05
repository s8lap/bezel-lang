//! case: s7_4_0916
//! doc: spec
//! section: 7.4
//! title: passing a narrowed ReadOnlyStringSpan? local as a ref argument invalidates its narrowing
//! rule: the variable is passed as a `ref` **or `out`** argument
//! cites: 7.4
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  static method Z(ref x: ReadOnlyStringSpan?)
    x := null;
  end Z;

  public static method Main()
    var w := "hello";
    var s: ReadOnlyStringSpan? := new ReadOnlyStringSpan(w, 0, 5);
    if s <> null then
      Z(ref s);
      Console.WriteLine(s.Length);
    end if;
  end Main;
end Program;
