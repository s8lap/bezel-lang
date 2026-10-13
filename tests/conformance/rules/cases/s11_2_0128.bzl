//! case: s11_2_0128
//! doc: spec
//! section: 11.2
//! title: a read-write indexer of an extending interface satisfies the read-only indexer it inherits
//! rule: §11.2: "A read-write member satisfies a read-only interface member of the same name and type: … a single read-write indexer in the implementing class satisfies both."
//!   IRW<T> extends IRO<T>. The one read-write indexer of K satisfies the inherited readonly indexer as well; a value written through IRW is read back through IRO.
//! cites: 11.2
//! expect: run
//! stdout: 8
module Conf;

public interface IRO<T>
  readonly property [i: int]: T;
end IRO;

public interface IRW<T>: IRO<T>
  property [i: int]: T;
end IRW;

public class K: IRW<int>
  public constructor() end constructor;

  private _v: int := 0;

  public property [i: int]: int
    get => _v;
    set => do _v := value; end;
  end;
end K;

public class C
  public static method Main(): int
    var w: IRW<int> := new K();
    w[0] := 8;
    var r: IRO<int> := w;
    Console.WriteLine(r[0]);
    return 0;
  end Main;
end C;
