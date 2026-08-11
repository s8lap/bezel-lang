public static pure method Checked(x: int, low: int, high: int): int
  if low > high then
    throw new ArgumentException("low must not exceed high", nameof(low));
  end;
  return (if x < low then low else (if x > high then high else x));
end Checked;
