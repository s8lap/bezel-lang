//! case: s28_9_7105
//! doc: spec
//! section: 28.9
//! title: in a generic method, a closure local assigned (`b := h;`) is stored on half the calls
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 9900
module Conf;
public class C
  public static method Fill<T>(fs: Array<(T) -> T>, k: int)
    var g: (T) -> T := lambda(t: T) => t;
    var b: (T) -> T := lambda(t: T) => t;
    var n := k;
    var h: (T) -> T := lambda(t: T) => g(t);
    b := h;
    if n mod 2 = 0 then
      fs.Add(b);
    end if;
  end Fill;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      var fs: Array<(int) -> int> := new Array<(int) -> int>();
      Fill<int>(fs, i);
      if fs.Count > 0 then
        s := s + fs[0](i);
      end if;
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
