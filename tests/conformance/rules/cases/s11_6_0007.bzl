//! case: s11_6_0007
//! doc: spec
//! section: 11.6
//! title: a chain head reaches a generic type's nested type through its static member and an
//!   unannotated local
//! rule: "Elsewhere a chain head reaches its static members, and a local declared without a type
//!   annotation holds a value of it (`var n := tree.Root();`)" (§11.6, v0.340).
//!   `C<int>.Make()` returns the nested `N` and `var n` holds it without naming `C.N`.
//! cites: 2.7
//! expect: run
//! stdout: 7
module Conf;

public class C<T>
  public class N
    public method V(): int
      return 7;
    end V;
  end N;

  public static method Make(): owned N
    return new N();
  end Make;
end C;

public class Program
  public static method Main()
    var n := C<int>.Make();
    Console.WriteLine(n.V());
  end Main;
end Program;
