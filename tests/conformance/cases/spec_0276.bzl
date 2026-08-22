throw new ArgumentException("Amount must be positive.", nameof(amount));
throw;          // re-throw the current exception (in a catch arm body only)
