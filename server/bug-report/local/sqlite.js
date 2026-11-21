import { DatabaseSync } from 'node:sqlite';

export function sqliteStore(file = ':memory:') {
  const db = new DatabaseSync(file);
  db.exec('PRAGMA journal_mode = WAL');
  const cache = new Map();
  const prepare = (query) => {
    let statement = cache.get(query);
    if (!statement) {
      statement = db.prepare(query);
      cache.set(query, statement);
    }
    return statement;
  };
  return {
    all: (query, ...args) => prepare(query).all(...args).map((row) => ({ ...row })),
    get: (query, ...args) => {
      const row = prepare(query).get(...args);
      return row ? { ...row } : undefined;
    },
    run: (query, ...args) => {
      prepare(query).run(...args);
    },
    transaction: (work) => {
      db.exec('BEGIN');
      try {
        const result = work();
        db.exec('COMMIT');
        return result;
      } catch (error) {
        db.exec('ROLLBACK');
        throw error;
      }
    },
    close: () => db.close(),
  };
}
