// Logical models only: this does NOT compile or execute the ESP32 firmware.
// Run with: node tests/simulate_safety.cjs (no additional dependencies).
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.join(__dirname, '..');
const tx = fs.readFileSync(path.join(root, 'control_tx/control_tx.ino'), 'utf8');
const rx = fs.readFileSync(path.join(root, 'robot_rx/robot_rx.ino'), 'utf8');
const protocol = fs.readFileSync(path.join(root, 'soccer_protocol.h'), 'utf8');
function constant(source, name) {
  const match = source.match(new RegExp(`constexpr \\w+ ${name} = (\\d+);`));
  assert.ok(match, `Missing decimal constant ${name}`);
  return Number(match[1]);
}
const block = constant(tx, 'SEQUENCE_BLOCK_SIZE');
const timeout = constant(rx, 'FAILSAFE_TIMEOUT_MS');
const ackTimeout = constant(tx, 'STATUS_TIMEOUT_MS');
const neutralMs = constant(rx, 'ARM_NEUTRAL_MS');
const deadzone = constant(rx, 'COMMAND_DEADZONE');
const step = constant(rx, 'MOTOR_RAMP_STEP');
const tick = constant(rx, 'MOTOR_UPDATE_MS');
const pause = constant(rx, 'MOTOR_REVERSE_PAUSE_MS');
const u32 = n => n >>> 0;
const age = (now, then) => u32(now - then);
const newer = (candidate, previous) => {
  const distance = age(candidate, previous);
  return distance !== 0 && distance < 0x80000000;
};
let groups = 0;
function check(name, run) {
  run();
  ++groups;
  console.log(`OK simulation: ${name}`);
}

check('modular sequence boundaries and every restart position in a block', () => {
  assert.equal(block, 10000);
  assert.equal(block / 50, 200);
  assert.ok(newer(0, 0xffffffff));
  assert.ok(!newer(42, 42));
  assert.ok(!newer(41, 42));
  assert.ok(!newer(0x80000000, 0));
  for (const start of [0, 1, 0xfffffff0, 0xffffd8f0]) {
    const persistedEnd = u32(start + block); // Must persist before any send.
    for (let used = 1; used <= block; ++used) {
      const lastSent = u32(start + used - 1);
      const rebootFirst = persistedEnd;
      assert.ok(newer(rebootFirst, lastSent));
      assert.equal(age(rebootFirst, lastSent), block - used + 1);
    }
    let persisted = start;
    const sent = new Set();
    for (let boot = 0; boot < 4; ++boot) {
      let next = persisted;
      let end = u32(next + block);
      persisted = end;
      for (let n = 0; n < block + 3; ++n) {
        assert.ok(!sent.has(next));
        sent.add(next);
        next = u32(next + 1);
        if (next === end) persisted = end = u32(end + block);
      }
    }
    // Simulated crash before/after a reservation write, with no block used.
    assert.equal(u32(start + block), persistedEnd);
    assert.ok(newer(u32(persistedEnd + block), persistedEnd));
  }
  // Existing v1 NVS key already contains an exclusive end; no extra reset.
  assert.ok(newer(51, 50));
  // Power loss at a boundary: a committed next reservation may skip a full
  // block; a failed write must halt before the first unreserved sequence.
  for (const writeSucceeds of [false, true]) {
    const lastSent = 0xffffffff;
    const oldEnd = 0;
    const committedEnd = writeSucceeds ? block : oldEnd;
    const halted = !writeSucceeds;
    const emittedAfterFailure = halted ? [] : [oldEnd];
    if (halted) assert.equal(emittedAfterFailure.length, 0);
    assert.ok(newer(committedEnd, lastSent));
    if (emittedAfterFailure.length) assert.ok(newer(committedEnd, emittedAfterFailure[0]));
  }
});

function calibrated(xs, ys) {
  const min = constant(tx, 'CALIBRATION_CENTER_MIN');
  const max = constant(tx, 'CALIBRATION_CENTER_MAX');
  const spread = constant(tx, 'CALIBRATION_MAX_SPREAD');
  return [xs, ys].every(values => {
    const center = Math.floor(values.reduce((a, b) => a + b) / values.length);
    return center >= min && center <= max && Math.max(...values) - Math.min(...values) <= spread;
  });
}
check('calibration range and raw spread on both axes', () => {
  const n = constant(tx, 'CALIBRATION_SAMPLES');
  const values = x => Array(n).fill(x);
  assert.ok(calibrated(values(1536), values(2560)));
  for (const x of [0, 1535, 2561, 4095]) {
    assert.ok(!calibrated(values(x), values(2048)));
    assert.ok(!calibrated(values(2048), values(x)));
  }
  const unstable = values(2048);
  unstable[0] += 101;
  assert.ok(!calibrated(unstable, values(2048)));
  assert.ok(!calibrated(values(2048), unstable));
  unstable[0] -= 1;
  assert.ok(calibrated(unstable, values(2048)));
});

// Model receiver callbacks separately from loop, including a latched stop.
class Receiver {
  constructor() {
    this.state = 'WAITING';
    this.phase = 'RELEASE';
    this.lastSeq = null;
    this.lastAt = 0;
    this.brakePending = true;
  }
  lose() { this.state = 'WAITING'; this.phase = 'RELEASE'; this.brakePending = true; }
  loop(now) {
    if (this.lastSeq === null || age(now, this.lastAt) > timeout) this.lose();
    const brake = this.brakePending || this.state !== 'ARMED';
    this.brakePending = false;
    return brake;
  }
  packet(seq, now, x = 0, y = 0, flags = 0, validEnvelope = true) {
    if (!validEnvelope || x < -1000 || x > 1000 || y < -1000 || y > 1000 ||
        (flags & ~1) || (this.lastSeq !== null && !newer(seq, this.lastSeq))) return false;
    if (this.lastSeq !== null && age(now, this.lastAt) > timeout) this.lose();
    this.lastSeq = seq;
    this.lastAt = now;
    const pressed = Boolean(flags & 1);
    if (this.state === 'WAITING') this.state = 'DISARMED';
    if (this.state === 'ARMED') {
      if (pressed) { this.state = 'DISARMED'; this.phase = 'RELEASE'; this.brakePending = true; }
      return true;
    }
    if (Math.abs(x) >= deadzone || Math.abs(y) >= deadzone) { this.phase = 'RELEASE'; return true; }
    if (this.phase === 'RELEASE') {
      if (!pressed) { this.since = now; this.phase = 'NEUTRAL'; }
    } else if (this.phase === 'NEUTRAL') {
      if (pressed) this.phase = age(now, this.since) >= neutralMs ? 'PRESSED' : 'RELEASE';
      else if (age(now, this.since) >= neutralMs) this.phase = 'READY';
    } else if (this.phase === 'READY') {
      if (pressed) this.phase = 'PRESSED';
    } else if (!pressed) { this.state = 'ARMED'; this.phase = 'RELEASE'; }
    return true;
  }
}
function gesture(r, time = 0) {
  let seq = u32((r.lastSeq ?? 0) + 1);
  for (let dt = 0; dt <= neutralMs; dt += 20) r.packet(seq++, u32(time + dt));
  r.packet(seq++, u32(time + neutralMs + 20), 0, 0, 1);
  r.packet(seq++, u32(time + neutralMs + 40));
  assert.equal(r.state, 'ARMED');
  return u32(time + neutralMs + 40);
}
check('arming, stop latch, tilted release, invalid input, timeout and reconnection', () => {
  const r = new Receiver();
  assert.ok(r.loop(0));
  r.packet(1, 0, 800);
  assert.equal(r.state, 'DISARMED');
  let now = gesture(r, 20);
  r.loop(now); // Consume initial brake latch.
  assert.equal(r.loop(now + 1), false);
  assert.equal(r.packet(r.lastSeq, now + 10), false);
  const seq = r.lastSeq;
  const lastAt = r.lastAt;
  for (const args of [[seq, now + 100], [seq - 1, now + 100],
    [seq + 1, now + 100, 1001], [seq + 1, now + 100, 0, -1001],
    [seq + 1, now + 100, 0, 0, 2], [seq + 1, now + 100, 0, 0, 0, false]]) {
    assert.equal(r.packet(...args), false);
    assert.equal(r.lastAt, lastAt);
  }
  r.packet(seq + 1, now + 20, 800, 0, 1);
  r.packet(seq + 2, now + 40, 800); // Release arrives before loop sees press.
  assert.equal(r.state, 'DISARMED');
  assert.ok(r.loop(now + 40));
  now = gesture(r, now + 60);
  assert.ok(r.loop(u32(now + timeout + 1)));
  r.packet(r.lastSeq + 1, u32(now + timeout + 20), 800);
  assert.equal(r.state, 'DISARMED');
  now = gesture(r, now + timeout + 40);
  // No intervening loop: the callback must still detect a receive gap.
  r.packet(r.lastSeq + 1, u32(now + timeout + 1));
  assert.equal(r.state, 'DISARMED');
  assert.ok(r.brakePending);
  const wrap = new Receiver();
  gesture(wrap, 0xffffff00); // millis() rollover during neutral dwell.
  const zero = new Receiver();
  assert.ok(zero.packet(0, 0)); // No zero sentinel for the first sequence.
  assert.equal(zero.state, 'DISARMED');
});
check('premature press and moving/lost-signal gestures cannot arm', () => {
  for (const axis of ['x', 'y']) {
    const r = new Receiver();
    r.packet(1, 0);
    r.packet(2, 20, 0, 0, 1);
    r.packet(3, 40);
    assert.equal(r.state, 'DISARMED');
    for (let t = 60; t <= 560; t += 20) r.packet(t, t);
    r.packet(600, 580, 0, 0, 1);
    r.packet(601, 600, axis === 'x' ? 50 : 0, axis === 'y' ? -50 : 0);
    r.packet(602, 620);
    assert.equal(r.state, 'DISARMED');
    r.packet(603, 640, 0, 0, 1);
    r.packet(604, 960);
    assert.equal(r.state, 'DISARMED');
  }
});

class Confirmation {
  constructor() { this.seq = null; this.at = 0; this.history = []; }
  sent(seq, at) {
    this.history.push({ seq, at });
    if (this.history.length > constant(tx, 'SENT_HISTORY_SIZE')) this.history.shift();
  }
  status(seq, now, valid = true) {
    if (!valid || (this.seq !== null && !newer(seq, this.seq)) ||
        !this.history.some(p => p.seq === seq && age(now, p.at) <= ackTimeout)) return false;
    this.seq = seq; this.at = now; return true;
  }
  display(now) { return this.seq === null ? 'WAITING' : age(now, this.at) > ackTimeout ? 'LOST' : 'CONFIRMED'; }
}
check('confirmation requires fresh accepted commands; stale statuses never extend time', () => {
  const a = new Confirmation();
  for (let i = 0; i < 50; ++i) a.sent(i, i * 20); // Radio ACKs cannot confirm.
  assert.equal(a.display(1000), 'WAITING');
  assert.equal(a.status(49, 1000, false), false);
  assert.equal(a.status(999, 1000), false);
  assert.equal(a.status(1, 1000), false);
  assert.ok(a.status(49, 1000));
  assert.equal(a.status(49, 1200), false);
  assert.equal(a.status(48, 1200), false);
  assert.equal(a.at, 1000);
  assert.equal(a.display(1301), 'LOST');
  a.sent(50, 1320);
  assert.ok(a.status(50, 1330));
  const reboot = new Confirmation();
  reboot.sent(10001, 1340);
  assert.equal(reboot.status(50, 1341), false);
  const wrap = new Confirmation();
  wrap.sent(0xffffffff, 0xfffffff0);
  assert.ok(wrap.status(0xffffffff, 0xfffffff1));
  wrap.sent(0, 10);
  assert.ok(wrap.status(0, 20));
});

class Ramp {
  constructor() { this.output = 0; this.direction = 0; this.hold = false; this.zeroAt = 0; }
  brake(now) { this.output = 0; this.direction = 0; this.hold = true; this.zeroAt = now; }
  update(target, now) {
    if (this.output === 0 && this.hold) {
      if (age(now, this.zeroAt) < pause) return 0;
      this.hold = false;
    }
    const reversing = target && this.direction && Math.sign(target) !== this.direction;
    const aim = reversing && this.output ? 0 : target;
    const before = this.output;
    this.output += Math.max(-step, Math.min(step, aim - this.output));
    if (this.output) this.direction = Math.sign(this.output);
    else if (before) { this.zeroAt = now; this.hold = true; }
    return this.output;
  }
}
check('ramp step, zero crossing/dwell and immediate stop at every inversion stage', () => {
  for (const sign of [-1, 1]) {
    const r = new Ramp();
    let now = 0;
    for (; now < 300; now += tick) r.update(sign * 1000, now);
    assert.equal(r.output, sign * 1000);
    let zeroAt;
    for (; now < 900; now += tick) {
      const before = r.output;
      const after = r.update(-sign * 1000, now);
      assert.ok(Math.abs(after - before) <= step);
      assert.ok(before * after >= 0);
      if (before && !after) zeroAt = now;
      if (Math.sign(after) === -sign) assert.ok(now - zeroAt >= pause);
      // Clone every stage, apply emergency stop, verify no pending motion.
      const stopped = Object.assign(new Ramp(), r);
      stopped.brake(now);
      assert.equal(stopped.output, 0);
      for (let dt = tick; dt <= 100; dt += tick) assert.equal(stopped.update(0, now + dt), 0);
    }
    assert.equal(r.output, -sign * 1000);
  }
  let seed = 123456789;
  const random = () => (seed = u32(Math.imul(seed, 1664525) + 1013904223));
  const r = new Ramp();
  let zeroAt = null;
  for (let i = 0; i < 10000; ++i) {
    const now = u32(0xffffff00 + i * tick);
    const before = r.output;
    const after = r.update((random() % 2001) - 1000, now);
    assert.ok(Math.abs(after - before) <= step);
    assert.ok(before * after >= 0);
    assert.ok(Math.abs(after) <= 1000);
    if (before && !after) zeroAt = now;
    if (!before && after && zeroAt !== null) assert.ok(age(now, zeroAt) >= pause);
  }
});

check('protocol v2 fixed sizes and CRC reference vector', () => {
  assert.equal(constant(protocol, 'SOCCER_PROTOCOL_VERSION'), 2);
  assert.match(protocol, /sizeof\(RemoteMsg\) == 18/);
  assert.match(protocol, /sizeof\(StatusMsg\) == 14/);
  assert.equal(4 + 1 + 1 + 2 + 4 + 2 + 2 + 2, 18);
  assert.equal(4 + 1 + 1 + 1 + 1 + 4 + 2, 14);
  let crc = 0xffff;
  for (const byte of Buffer.from('123456789')) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; ++bit) crc = ((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0)) & 0xffff;
  }
  assert.equal(crc, 0x29b1);
});
console.log(`${groups} logical simulation groups passed; firmware compilation and hardware tests remain pending.`);
