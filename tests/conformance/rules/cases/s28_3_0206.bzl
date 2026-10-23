//! case: s28_3_0206
//! doc: spec
//! section: 28.3
//! title: a literal given to a plain primary-constructor parameter faults when read after the temporary ends
//! rule: "an unbound owned temporary or a borrow of a scope-bound local, passed to a plain parameter that the callee stores ... is admitted at the call ... **Neither source is lifetime-extended**: the temporary is disposed at its K8 boundary and the local at its scope's end, and a member access through the stored borrow after that point is the backstop's fault; **a temporary meets the obligation only when the object dies within the same full expression**" (§28.3 *Borrow lifetime rules*, clause 3, v0.340); the backstop is checked disposal (K4.1, §28.2): a member access on a logically disposed instance raises `DisposedAccessException`. Owner ruling #7045. KNOWN DIVERGENCE (#7045): the compiler reads the freed array (or an empty one) instead of raising. The primary constructor's synthesized capture field (§22.10) is the same plain-parameter store as an explicit `_xs := xs`.
//! cites: 22.10, 28.2
//! expect: run
//! stdout: faulted
//! flags: --checked-disposal
//! issue: 7045
module Conf;

public class A(xs: Array<int>)
  public method Sum(): int
    var s := 0;
    for var x in xs do s := s + x; end;
    return s;
  end Sum;
end A;

public class H
  private owned _a: A;

  public constructor()
    _a := new A([4, 5, 6]);
  end;

  public method Go(): int => _a.Sum();
end H;

public class Program
  public static method Main()
    var h := new H();
    try
      Console.WriteLine(h.Go());
    catch
      DisposedAccessException => Console.WriteLine("faulted"),
    end try;
  end Main;
end Program;
