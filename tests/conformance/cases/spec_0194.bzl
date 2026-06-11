using var sb := new StringBuilder();
sb.Append("hello").Append(" ").Append("world");    // each Append returns a borrow of sb
