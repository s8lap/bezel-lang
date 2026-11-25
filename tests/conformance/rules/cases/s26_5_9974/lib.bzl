module Lib;

public extension Array<int>
  public method Sum2(): int
    var s := 0;
    for var x in this do
      s := s + x;
    end;
    return s * 2;
  end Sum2;
end;
