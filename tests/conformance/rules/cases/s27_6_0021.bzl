//! case: s27_6_0021
//! doc: spec
//! section: 27.6
//! title: a method reference to a class-only member is accepted on the class-typed receiver
//! rule: "If `x`'s declared type (or any base type) *declares* any instance member named `Foo` ... instance resolution is
//!   committed" (§26.5 rule 1, v0.340); a bound method reference `obj.Method` (§27.6) over the class-typed `m` reaches
//!   `MyList.Extra`, a member the class declares beyond the interface.
//! cites: 26.5, 4.1
//! expect: run
//! stdout: 42
module Conf;

public class MyList: IList<int>
  private _a: Array<int> := [1, 2, 3];
  public property Count: int => _a.Count;
  public property IsReadOnly: bool => false;
  public property [i: int]: int
    get => _a[i];
    set => do _a[i] := value; end;
  end;
  public method Add(item: int) _a.Add(item); end Add;
  public method Clear() _a.Clear(); end Clear;
  public method Contains(item: int): bool return _a.Contains(item); end Contains;
  public method Remove(item: int): bool return _a.Remove(item); end Remove;
  public method Insert(index: int, item: int) _a.Insert(index, item); end Insert;
  public method GetEnumerator(): IEnumerator<int> return _a.GetEnumerator(); end GetEnumerator;
  public method Extra(): int return 42; end Extra;
end MyList;

public class Program
  public static method Main()
    var m := new MyList();
    var f: () -> int := m.Extra;
    Console.WriteLine(f());
  end Main;
end Program;
