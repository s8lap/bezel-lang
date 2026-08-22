case move outcome of
  Ready(move conn = var c, attempts = var n) => do
    using var live := move c;
    Console.WriteLine($"connected after {n} tries");
  end,
  Empty => ()
end;
