module Lib;
public class L
  public static method F(): int
    using var s := new SortedSet<Guid>();
    s.Add(Guid.Parse("22222222-2222-2222-2222-222222222222"));
    s.Add(Guid.Parse("11111111-1111-1111-1111-111111111111"));
    var f := s.First;
    Console.WriteLine(f);
    return 0;
  end F;
end L;
