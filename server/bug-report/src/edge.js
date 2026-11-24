// Per-address limits group IPv6 clients by their /64. No Cloudflare import, so Node runs it in the tests.
export function ipGroup(ip) {
  if (typeof ip !== 'string' || !ip.includes(':')) return String(ip);
  const mapped = /^::ffff:(\d{1,3}(?:\.\d{1,3}){3})$/i.exec(ip);
  if (mapped) return mapped[1];
  const [head, tail] = ip.split('::');
  const before = head ? head.split(':') : [];
  const after = tail ? tail.split(':') : [];
  const gap = tail === undefined ? [] : Array(Math.max(0, 8 - before.length - after.length)).fill('0');
  return [...before, ...gap, ...after].slice(0, 4).map((g) => g.toLowerCase().replace(/^0+(?=.)/, '')).join(':');
}
