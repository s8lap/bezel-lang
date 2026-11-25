//! case: s28_9_9868
//! doc: spec
//! section: 28.9
//! title: A closure passed to imported abstract, virtual, plain and async members is callable afterwards
//! rule: Function-typed fields, parameters, and locals are not subject to `owned` annotations and carry no disposal obligation
//! cites: 28.9, 28.14, 29.4, 2.8.2
//! expect: run
//! stdout: 50400
//! notes: Harvested from the #9309 review (r43-rev-m147-9309, probes/mod/). The members are declared
//!   in another module (the callee's declaration reaches the caller through the artifact
//!   interface, 2.8.2), and the callee is an abstract member overridden here by an iterator, an
//!   imported virtual iterator, an imported plain iterator and an imported async method. Each call
//!   hands the caller's closure local to a member that mints a deferred frame the caller returns
//!   (or awaits); the closure must be callable when the frame reads it. Trace per round, c = 42:
//!   M yields 42 + 42 = 84, V yields 42, D yields 42 + 42 = 84, A returns 42: 252; 200 rounds =
//!   50400.
module AppIt;
import LibIt;

public class Sub : LibIt.Fl
  public override method M(q: () -> int): IEnumerable<int>
    yield return q();
    yield return q();
  end M;
end Sub;

public class Program
  public static method MakeM(fl: LibIt.Fl): IEnumerable<int>
    var c := 42;
    var g := lambda() => c;
    return fl.M(g);
  end MakeM;

  public static method MakeV(fl: LibIt.Fl): IEnumerable<int>
    var c := 42;
    var g := lambda() => c;
    return fl.V(g);
  end MakeV;

  public static method MakeD(fl: LibIt.Fl): IEnumerable<int>
    var c := 42;
    var g := lambda() => c;
    return fl.D(g);
  end MakeD;

  public static method MakeA(fl: LibIt.Fl): Task<int>
    var c := 42;
    var g := lambda() => c;
    return fl.A(g);
  end MakeA;

  public static async method Main(): Task<int>
    var t := 0;
    var fl: LibIt.Fl := new Sub();
    for var j in 0 ..< 200 do
      for var x in MakeM(fl) do t := t + x; end;
      for var x in MakeV(fl) do t := t + x; end;
      for var x in MakeD(fl) do t := t + x; end;
      t := t + await MakeA(fl);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
