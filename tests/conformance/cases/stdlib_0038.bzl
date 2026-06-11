public method Build(): owned OwnedArray<Connection>
  return [
    Connect("a"),                          // each Connect returns owned Connection
    Connect("b"),
    Connect("c")
  ];                                       // collection literal targets OwnedArray<Connection>
end Build;
