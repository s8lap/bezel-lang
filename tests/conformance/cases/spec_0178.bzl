var attach: (owned EventHandler) -> () := lambda(owned h) => _handlers.Add(h);
