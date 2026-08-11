public pure method Describe(): string
  Console.WriteLine("describing…");   // ERROR: callee is not pure
  _hits := _hits + 1;                 // ERROR: store whose root is a field of this
  return $"{_count} items";           // fine: reads and string building
end Describe;
