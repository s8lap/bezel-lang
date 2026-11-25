//! case: s14_1_9308
//! doc: spec
//! section: 14.1
//! title: a generic class draining through a nested generic class reads the string key at K = string
//! rule: **Instantiation and its site.** An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 14.1, 4.9, 28.9
//! expect: run
//! output: a199:x;
//! notes: Harvested from the #9050 review (r3_nested_generic), reduced to one entry. `Outer<A>` holds an
//!   `Inner<A>`, so `Inner`'s `K` is substituted by `Outer`'s own parameter, which `Main` instantiates at
//!   string: the substitution composes through two levels. Trace: the last iteration (i = 199) puts "a199" ->
//!   Conn("x"); `Go` returns the one drained entry's line, "a199:x;".
module Conf;
public class Conn
  public Name: string;
  public constructor(n: string)
    Name := n;
  end constructor;
  destructor
  end destructor;
end Conn;
public class Inner<K> where K: hashable
  private owned _d: OwningDictionary<K, Conn> := new OwningDictionary<K, Conn>();
  public method Put(k: K, owned c: Conn) => _d[k] := c;
  public method Go(): string
    var s := "";
    for var e in _d.Drain() do
      using var x := move e;
      s := s + $"{x.Key}:{x.Value.Name};";
    end for;
    return s;
  end Go;
end Inner;
public class Outer<A> where A: hashable
  private owned _in: Inner<A> := new Inner<A>();
  public method Put(k: A, owned c: Conn) => _in.Put(k, c);
  public method Go(): string => _in.Go();
end Outer;
public class Program
  public static method Main(): int
    var last := "";
    var i := 0;
    while i < 200 do
      using var r := new Outer<string>();
      r.Put($"a{i}", new Conn("x"));
      last := r.Go();
      i := i + 1;
    end while;
    Console.WriteLine(last);
    return 0;
  end Main;
end Program;
