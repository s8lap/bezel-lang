var classify := lambda(score: int): string? => do
  if score < 0 then return null; end;
  if score >= 90 then return "A"; end;
  return "pass";
end;
