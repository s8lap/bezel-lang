//! case: s28_3_0205
//! doc: spec
//! section: 28.3
//! title: a literal given to a plain constructor parameter and stored faults when read after it ends
//! rule: "an unbound owned temporary or a borrow of a scope-bound local, passed to a plain parameter that the callee stores ... is admitted at the call ... **Neither source is lifetime-extended**: the temporary is disposed at its K8 boundary and the local at its scope's end, and a member access through the stored borrow after that point is the backstop's fault; **a temporary meets the obligation only when the object dies within the same full expression**" (§28.3 *Borrow lifetime rules*, clause 3, v0.340); the backstop is checked disposal (K4.1, §28.2): a member access on a logically disposed instance raises `DisposedAccessException`. Owner ruling #7045. KNOWN DIVERGENCE (#7045, default build only): in checked disposal mode the backstop raises `DisposedAccessException`, which is what §28.2 requires, so this row runs with `--checked-disposal` — the compiler's default is still unchecked (`CodegenOptions::checkedDisposal = false`) and #4602's default flip is still open. In the default build the compiler still reads the freed array instead of raising (the reviewer measured the ASan heap-use-after-free on the tip): §28.2 says the default *is* checked, the implementation is not, and the `flags` column is currently carrying the spec's default. When the default flips, the flags come off. 
//! cites: 22.10, 28.2
//! expect: run
//! stdout: faulted
//! flags: --checked-disposal
//! issue: 7045
module Conf;

public class B
  private _xs: Array<int>;

  public constructor(xs: Array<int>)
    _xs := xs;
  end;

  public method Sum(): int
    var s := 0;
    for var x in _xs do s := s + x; end;
    return s;
  end Sum;
end B;

public class Program
  public static method Main()
    var h: B? := null;
    do
      h := new B([1, 2, 3]);
    end;
    try
      Console.WriteLine(h!.Sum());
    catch
      DisposedAccessException => Console.WriteLine("faulted"),
    end try;
  end Main;
end Program;
