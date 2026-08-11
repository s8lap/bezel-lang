public static method Tail(lines: Array<string>, n: int): IEnumerable<string>
  for var i in Math.Max(0, lines.Count - n)..<lines.Count do
    yield return lines[i];
  end;
end Tail;
