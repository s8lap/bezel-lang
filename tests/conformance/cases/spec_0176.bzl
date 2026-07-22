public method MakeCounter(): () -> int
  var count := 0;
  return lambda() => do
    count := count + 1;
    return count;
  end;
end MakeCounter;
