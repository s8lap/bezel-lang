public static method Squares(n: int): IEnumerable<int>
  for var i in 0..<n do
    yield return i * i;
  end;
end Squares;

public static method TakeWhilePositive(numbers: IEnumerable<int>): IEnumerable<int>
  for var n in numbers do
    if n <= 0 then
      yield break;
    end;
    yield return n;
  end;
end TakeWhilePositive;
