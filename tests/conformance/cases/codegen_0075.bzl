var label := ParsePort(raw).UnwrapOr(BuildExpensiveMessage(raw));   // runs on the Ok path too
