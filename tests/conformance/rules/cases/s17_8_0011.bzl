//! case: s17_8_0011
//! doc: spec
//! section: 17.8
//! title: a var bound to a null-forgiving property read over a stored array is iterated
//! rule: A property read returning a borrow of the holder's stored collection, asserted non-null with !, is borrowed with the same source and is iterated without disposing the holder's collection (spec 17.8); 50 iterations sum 3 + 4 + 5 = 12 each (or the V fields 3, 4, 5).
//! cites: 17.8, 28.3
//! expect: run
//! output: total=600
module Conf;
interface ISeq
  readonly property E: IEnumerable<int>?;
end ISeq;
class Holder : ISeq
  private owned _a: OwningArray<Obj>;
  private owned _e: Array<int>;
  public constructor()
    _a := new OwningArray<Obj>();
    _e := [3, 4, 5];
  end constructor;
  public property E: IEnumerable<int>?
    get => do
      return _e;
    end;
  end E;
end Holder;
class Obj
  public constructor() end constructor;
end Obj;
class P
  public static method Main(): int
    var total := 0;
    for var i in 1..50 do
      using var h := new Holder();
      var f: ISeq := h;
      var e := f.E!; for var x in e do total := total + x; end;
    end;
    Console.WriteLine("total=" + total.ToString());
    return 0;
  end Main;
end P;
