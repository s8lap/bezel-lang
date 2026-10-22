//! case: s27_6_0022
//! doc: spec
//! section: 27.6
//! title: a bare reference to a class-only member is rejected on an IList-typed receiver
//! rule: "the member resolved on the value exactly as the call `v.Scaled()` is" (§27.6, v0.340): the call `il.Extra()` over
//!   `il: IList<int>` is rejected, since `IList<T>` declares no `Extra` (stdlib §4.1) and no extension applies (§26.5),
//!   so the bare reference `il.Extra` is rejected the same way even though the class behind the value declares it.
//! cites: 26.5, 4.1
//! expect: reject
//! diagnostic: extra
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
    var il: IList<int> := new MyList();
    var f: () -> int := il.Extra;
    Console.WriteLine(f());
  end Main;
end Program;
