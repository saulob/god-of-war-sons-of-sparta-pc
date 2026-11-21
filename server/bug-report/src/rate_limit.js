export const LIMITS = {
  fileAddress: { burst: 10, perSecond: 1 / 30 },
  dumpAddress: { burst: 200, perSecond: 15 },
  lookupAddress: { burst: 20, perSecond: 1 / 10 },
};

export class RateLimiter {
  constructor({ burst, perSecond }, maxKeys = 4096) {
    this.burst = burst;
    this.perSecond = perSecond;
    this.maxKeys = maxKeys;
    this.buckets = new Map();
  }

  allow(key, now) {
    if (this.buckets.size >= this.maxKeys && !this.buckets.has(key)) {
      const oldest = this.buckets.keys().next().value;
      if (oldest !== undefined) this.buckets.delete(oldest);
    }
    const prev = this.buckets.get(key) ?? { tokens: this.burst, at: now };
    const elapsed = Math.max(0, (now - prev.at) / 1000);
    const tokens = Math.min(this.burst, prev.tokens + elapsed * this.perSecond);
    if (tokens < 1) {
      this.buckets.set(key, { tokens, at: now });
      return false;
    }
    this.buckets.set(key, { tokens: tokens - 1, at: now });
    return true;
  }
}

export function ipGroup(address) {
  if (typeof address !== 'string' || !address) return 'unknown';
  if (address.includes(':')) return address.split(':').slice(0, 4).join(':');
  return address;
}
