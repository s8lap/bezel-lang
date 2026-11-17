//! case: s28_9_4702
//! doc: spec
//! section: 28.9
//! title: a generic holder's block-form getter over an owned field is a lend read by a for loop
//! rule: a getter never transfers a reclamation obligation to its reader
//! cites: 28.9, 28.6, 23.3
//! expect: run
//! stdout: s=1200
//! issue: 6447
module Conf;

public class SelfA: IEnumerable<int>, IEnumerator<int>
  private cur: int := 0;
  private hi: int;

  public constructor(hi: int)
    this.hi := hi;
  end constructor;

  public method GetEnumerator(): IEnumerator<int>
    return this;
  end GetEnumerator;

  public method MoveNext(): bool
    cur := cur + 1;
    return cur <= hi;
  end MoveNext;

  public property Current: int get => cur;
  end Current;
end SelfA;

public class Holder<T>
  private owned _f: T;

  public constructor(owned f: T)
    _f := f;
  end constructor;

  public property Seq: T get => do
    var t := _f;
    return t;
  end;
  end Seq;
end Holder;

public class Program
  public static method Main()
    var s := 0;
    for var i in 0..<200 do
      var h := new Holder<SelfA>(new SelfA(3));
      for var x in h.Seq do
        s := s + x;
      end;
    end for;
    Console.WriteLine($"s={s}");
  end Main;
end Program;
