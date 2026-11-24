module Algo.Heap;

public class MinHeap<T> where T: IComparable<T>
  private owned _items: Array<T> := new Array<T>();

  public property Count: int => _items.Count;

  public method Push(item: T)
    _items.Add(item);
    var i := _items.Count - 1;
    while i > 0 do
      var parent := (i - 1) / 2;
      if _items[i].CompareTo(_items[parent]) >= 0 then break; end;
      Swap(i, parent);
      i := parent;
    end;
  end Push;

  public method Pop(): T
    requires(_items.Count > 0)

    var top := _items[0];
    var last := _items.Count - 1;
    _items[0] := _items[last];
    _items.RemoveAt(last);
    var i := 0;
    while true do
      var l := 2 * i + 1;
      var r := l + 1;
      var smallest := i;
      if l < _items.Count and _items[l].CompareTo(_items[smallest]) < 0 then smallest := l; end;
      if r < _items.Count and _items[r].CompareTo(_items[smallest]) < 0 then smallest := r; end;
      if smallest = i then break; end;
      Swap(i, smallest);
      i := smallest;
    end;
    return top;
  end Pop;

  public method PeekOrNull(): T? => (if _items.Count = 0 then null else _items[0]);

  private method Swap(a: int, b: int)
    var t := _items[a];
    _items[a] := _items[b];
    _items[b] := t;
  end Swap;
end MinHeap;

public record Job(Priority: int, Name: string): IComparable<Job>
  public method CompareTo(other: Job): int
    var c := Priority.CompareTo(other.Priority);
    return (if c <> 0 then c else Name.CompareTo(other.Name));
  end CompareTo;
end Job;

public class Program
  public static method Main()
    using var h := new MinHeap<int>();
    var xs: Array<int> := [5, 3, 8, 1, 9, 2];
    for var x in xs do h.Push(x); end;
    var out := new StringBuilder();
    while h.Count > 0 do
      out.Append(h.Pop()).Append(' ');
    end;
    Console.WriteLine(out.ToString().Trim());

    using var jobs := new MinHeap<Job>();
    jobs.Push(new Job(2, "b"));
    jobs.Push(new Job(1, "z"));
    jobs.Push(new Job(2, "a"));
    jobs.Push(new Job(0, "q"));
    var names := new Array<string>();
    while jobs.Count > 0 do
      names.Add(jobs.Pop().Name);
    end;
    Console.WriteLine(String.Join(" ", names));
    Console.WriteLine(jobs.PeekOrNull()?.Name ?? "empty");
  end Main;
end Program;
