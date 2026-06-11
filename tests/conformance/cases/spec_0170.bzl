var attach: (owned EventHandler) -> () := (owned h) => _handlers.Add(h);
