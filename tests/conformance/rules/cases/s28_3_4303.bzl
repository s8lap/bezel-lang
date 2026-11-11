//! case: s28_3_4303
//! doc: spec
//! section: 28.3
//! title: a nullable owned IEnumerator parameter disposes a generator and a user class, and nothing for null
//! rule: it is disposed implicitly at that scope end (for an `owned` parameter, at the end of the method)
//! cites: 28.3.10, 28.1, 29.7
//! expect: run
//! output: True
//!   fin
//!   True
//!   fin
//!   end
//! issue: 7961
module Conf;

class G
  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
      yield return 2;
    finally
      Console.WriteLine("fin");
    end try;
  end Gen;
end G;

class Y
  public static method Take(owned e: IEnumerator<int>?)
    if e <> null then
      Console.WriteLine(e!.MoveNext());
    end if;
  end Take;
end Y;

public class Program
  public static method Main()
    Y.Take(G.Gen().GetEnumerator());
    Y.Take(null);
    Y.Take(G.Gen().GetEnumerator());
    Console.WriteLine("end");
  end Main;
end Program;
