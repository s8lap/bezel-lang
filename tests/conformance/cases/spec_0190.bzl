var sum := add(2, 3);

var maybeHandler: ((int) -> ())? := null;
maybeHandler?(42);                      // null-conditional call; no-op when null
