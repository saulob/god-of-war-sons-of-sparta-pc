// In-memory abuse limits of the reporting service. Nothing here is written to SQL: a limit that cost a write per hit
// would defeat its purpose.
export const PROTECTION = {
  any: { burst: 60, perSecond: 20 },
  fileAll: { burst: 40, perSecond: 1 / 3 },
  fileClient: { burst: 8, perSecond: 1 / 60 },
  fileAddress: { burst: 16, perSecond: 1 / 30 },
  lookupAll: { burst: 200, perSecond: 2 },
  statusAddress: { burst: 20, perSecond: 1 / 15 },
  readAddress: { burst: 10, perSecond: 1 / 30 },
  feedbackAddress: { burst: 10, perSecond: 1 / 30 },
  dumpAddress: { burst: 12, perSecond: 1 / 20 },
  refusedRequests: { burst: 30, perSecond: 1 / 2 },
  preAuthFrames: 6,
  trackedKeys: 4096,
};

export class RateLimiter {
  constructor(spec, maxKeys = PROTECTION.trackedKeys) {
    this.spec = spec;
    this.maxKeys = maxKeys;
    this.buckets = new Map();
  }

  tokens(key, now) {
    const [tokens, last] = this.buckets.get(key) ?? [this.spec.burst, now];
    return Math.min(this.spec.burst, tokens + (Math.max(0, now - last) / 1000) * this.spec.perSecond);
  }

  allows(key, now) {
    return this.tokens(key, now) >= 1;
  }

  take(key, now) {
    const tokens = this.tokens(key, now);
    if (tokens < 1) return false;
    this.buckets.delete(key);
    if (this.buckets.size >= this.maxKeys) this.prune(now);
    this.buckets.set(key, [tokens - 1, now]);
    return true;
  }

  // Idle keys are as good as new ones; if every key is busy the oldest go.
  prune(now) {
    for (const key of [...this.buckets.keys()]) {
      if (this.tokens(key, now) >= this.spec.burst) this.buckets.delete(key);
    }
    for (const key of this.buckets.keys()) {
      if (this.buckets.size < this.maxKeys) break;
      this.buckets.delete(key);
    }
  }
}
