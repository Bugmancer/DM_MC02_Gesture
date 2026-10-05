import assert from "node:assert/strict";
import { AirPenModel } from "../static/air-pen.js";
import { Quaternion, Vector3 } from "../static/vendor/three.module.js";

const identity = [1, 0, 0, 0];
const sample = (
  seq,
  key = false,
  p = [0, 0, 0],
  q = identity,
  t = seq * 20,
) => ({ seq, t, q, p, key_down: key });
const state = (samples, overrides = {}) => ({
  connection: { state: "connected", epoch: 1 },
  pose: { available: true, status: "tracking" },
  key: { available: true, down: samples.at(-1)?.key_down ?? false },
  pen_epoch: 1,
  pen_cursor: samples.at(-1)?.seq || 0,
  pen_samples: samples,
  training: { state: "idle" },
  ...overrides,
});
const ready = () => {
  const model = new AirPenModel();
  model.setVisible(true);
  model.update(state([sample(1)]));
  return model;
};
const rtsSample = (seq, key = false, p = [0, 0, 0], strokeId = 7) => ({
  ...sample(seq, key, p), stroke_id: strokeId,
});
const rtsState = (samples, overrides = {}) => state(samples, {
  server_instance: "server-a",
  pen_motion: { available: true, blocked: false, algorithm: "gaitmap-eskf-rts" },
  pen_corrections: [],
  ...overrides,
});
const readyRts = () => {
  const model = new AirPenModel();
  model.setVisible(true);
  model.update(rtsState([rtsSample(1)]));
  return model;
};
const correction = {
  stroke_id: 7,
  points: [[0, 0, 0], [0.05, 0.01, 0.02]],
};

{
  const model = ready();
  model.update(state([sample(2, false, [0.1, 0.2, 0.3])]));
  assert.equal(model.strokes.length, 0, "KEY up cannot create ink");
  assert.deepEqual(model.cursor, [0.1, 0.2, 0.3]);
  model.update(
    state([
      sample(3, true, [0.1, 0.2, 0.3]),
      sample(4, true, [0.2, 0.3, 0.4]),
      sample(5, false, [0.3, 0.4, 0.5]),
      sample(6, true, [0.4, 0.5, 0.6]),
      sample(7, false),
    ]),
  );
  assert.equal(
    model.strokes.length,
    2,
    "KEY edges inside one poll preserve separate strokes",
  );
  assert.equal(model.strokes[0].points.length, 2, "release sample never draws");
  assert.deepEqual(model.strokes[0].points[1], [0.2, 0.3, 0.4]);
  assert.deepEqual(model.strokes[1].points[0], [0.4, 0.5, 0.6]);
  assert.equal(model.recording, false);
}

{
  const model = new AirPenModel();
  model.setVisible(true);
  model.update(state([sample(1), sample(2, true), sample(3, true)]));
  model.update(state([sample(4, true)]));
  assert.equal(
    model.strokes.length,
    0,
    "entering while held cannot replay history or start ink",
  );
  model.update(state([sample(5), sample(6, true)]));
  assert.equal(model.strokes.length, 1);
  model.setVisible(false);
  model.update(state([sample(7), sample(8, true)]));
  model.setVisible(true);
  model.update(state([sample(9, true)]));
  assert.equal(
    model.strokes.length,
    1,
    "returning while held needs a new release",
  );
}

{
  const model = ready();
  model.update(state([sample(2, true)]));
  model.update(state([sample(4, true, [1, 1, 1])]));
  assert.equal(model.recording, false, "lost history cannot bridge a stroke");
  model.update(state([sample(5, true)]));
  assert.equal(
    model.strokes.length,
    1,
    "held KEY cannot restart after interruption",
  );
  model.update(state([sample(6), sample(7, true)]));
  assert.equal(model.strokes.length, 2);
  model.update(
    state([sample(8, true)], {
      connection: { state: "disconnected", epoch: 1 },
    }),
  );
  assert.equal(model.recording, false);
  model.update(state([sample(9, true)]));
  assert.equal(
    model.strokes.length,
    2,
    "reconnect while held does not restart",
  );
  model.expire();
  assert.equal(model.available, false, "stalled polling disables capture");
}

{
  const model = ready();
  model.update(state([sample(2, true)]));
  model.update(state([sample(3, true)], { pen_epoch: 2 }));
  assert.equal(model.recording, false, "origin reset lifts pen");
  model.update(
    state([sample(4), sample(5, true)], {
      pen_epoch: 2,
      training: { state: "recording" },
    }),
  );
  assert.equal(
    model.strokes.length,
    1,
    "KEY learning and drawing are mutually exclusive",
  );
  model.update(state([sample(6, true)], { pen_epoch: 2 }));
  assert.equal(model.strokes.length, 1);
  model.update(
    state([sample(7), sample(8, true)], {
      pen_epoch: 2,
      key: { available: false, down: null },
    }),
  );
  assert.equal(
    model.strokes.length,
    1,
    "legacy firmware cannot infer KEY from movement",
  );
}

{
  const model = ready();
  model.update(state([sample(2, true)]));
  const q = new Quaternion().setFromAxisAngle(
    new Vector3(0, 1, 0),
    Math.PI / 2,
  );
  model.update(state([sample(3, true, [0.1, 0.2, 0.3], [q.w, q.x, q.y, q.z])]));
  assert.deepEqual(
    model.cursor,
    [0.1, 0.2, 0.3],
    "board rotation cannot rotate or offset world-space displacement",
  );
  model.update(state([sample(4, true, [0.1, 0.2, 0.3], identity)]));
  assert.deepEqual(
    model.cursor,
    [0.1, 0.2, 0.3],
    "pure rotation leaves the world cursor fixed",
  );
  assert.equal(
    model.strokes[0].points.length,
    2,
    "pure rotation cannot draw a line",
  );
  model.update(state([sample(5, true, [NaN, 0, 0])]));
  assert.equal(
    model.recording,
    false,
    "bad coordinates cannot enter saved geometry",
  );
  model.update(
    state([sample(6), sample(7, true)], {
      pose: { available: true, status: "gap", position_limited: true },
    }),
  );
  assert.equal(
    model.recording,
    true,
    "position limit still permits current orientation",
  );
}

{
  const model = new AirPenModel();
  model.setVisible(true);
  model.update(state([sample(1, false, [0, 0, 0], identity, 0xfffffff0)]));
  model.update(state([sample(2, true, [0, 0, 0], identity, 4)]));
  assert.equal(model.recording, true, "uint32 rollover remains continuous");
  model.update(state([sample(3, true, [0, 0, 0], identity, 1000)]));
  assert.equal(model.recording, false, "sensor timestamp gaps lift pen");
}

{
  const model = ready();
  model.update(
    state([sample(2, true), sample(3, true, [0.1, 0.2, 0.3]), sample(4)]),
  );
  const saved = model.serialize();
  const exported = JSON.parse(saved);
  assert.equal(exported.version, 4);
  assert.equal(exported.reference_frame.up_axis, "z");
  assert.equal(exported.reference_frame.heading, "relative-yaw");
  assert.equal("tip_length_m" in exported, false);
  model.undo();
  assert.equal(model.strokes.length, 0);
  model.redo();
  assert.equal(model.serialize(), saved, "undo/redo preserves XYZ geometry");
  const restored = new AirPenModel();
  assert.equal(restored.restore(saved), true);
  assert.deepEqual(restored.strokes, model.strokes);
  assert.equal(restored.recording, false);
  assert.equal(
    restored.restore('{"version":1,"strokes":[]}'),
    false,
    "2D drafts never become 3D geometry",
  );
  assert.equal(
    restored.restore(JSON.stringify({ ...exported, version: 3 })),
    false,
    "old board-reference drafts cannot mix with world coordinates",
  );
  const bad = JSON.parse(saved);
  bad.strokes[0].points[0] = [0, 0];
  assert.equal(restored.restore(JSON.stringify(bad)), false);
  model.clear();
  assert.equal(model.strokes.length, 0);
  assert.equal(model.redoStrokes.length, 0);
}

{
  const model = ready();
  model.strokes = [
    {
      color: "#252a2c",
      width: 3,
      frame: "1:1",
      points: Array.from({ length: 39999 }, () => [0, 0, 0]),
    },
  ];
  model.update(state([sample(2, true), sample(3, true, [0.1, 0.1, 0.1])]));
  assert.equal(
    model.pointCount,
    40000,
    "starting the last permitted stroke cannot exceed the point bound",
  );
  assert.equal(model.recording, false);
  const missingKey = { ...sample(4), key_down: null };
  model.clear();
  model.update(state([missingKey]));
  model.update(state([sample(5, true)]));
  assert.equal(
    model.strokes.length,
    0,
    "missing per-sample KEY requires a new valid release",
  );
}

{
  const model = ready();
  model.update(
    state([sample(2, true)], {
      pen_motion: { available: true, blocked: false },
    }),
  );
  assert.equal(model.recording, true);
  model.update(
    state([sample(3, true, [4, 5, 6])], {
      pen_motion: { available: true, blocked: true, reason: "duration_limit" },
    }),
  );
  assert.equal(model.recording, false, "backend drift guard lifts the pen");
  assert.equal(
    model.strokes[0].points.length,
    1,
    "blocked samples cannot extend ink",
  );
  model.update(
    state([sample(4, true)], {
      pen_motion: { available: true, blocked: false },
    }),
  );
  assert.equal(
    model.strokes.length,
    1,
    "release is mandatory after a drift guard",
  );
  model.update(
    state([sample(5), sample(6, true)], {
      pen_motion: { available: false, blocked: false },
    }),
  );
  assert.equal(
    model.recording,
    false,
    "world motion freshness overrides unrelated spatial pose availability",
  );
}

{
  const model = ready();
  model.update(state([sample(2, true, [0.2, 0.4, 0.6])]));
  model.sensitivity = 2.5;
  model.update(state([sample(3, true, [0.2, 0.4, 0.6])]));
  assert.deepEqual(
    model.cursor,
    [0.2, 0.4, 0.6],
    "view zoom cannot rescale world-space meters",
  );
  assert.equal(
    model.strokes[0].points.length,
    1,
    "changing view zoom does not create a line",
  );
  model.update(state([sample(4, true, [0.3, 0.5, 0.7])]));
  assert.deepEqual(model.strokes[0].points[1], [0.3, 0.5, 0.7]);
  assert.deepEqual(
    JSON.parse(model.serialize()).strokes[0].points,
    [
      [0.2, 0.4, 0.6],
      [0.3, 0.5, 0.7],
    ],
    "export geometry remains measured world coordinates at any zoom",
  );
}

{
  const model = readyRts();
  model.update(rtsState([rtsSample(2, true), rtsSample(3, true, [0.1, 0.2, 0.3])]));
  const stroke = model.strokes[0];
  assert.equal(stroke.completed, false);
  assert.equal(stroke.algorithm, "gaitmap-eskf-rts");
  const release = rtsState([rtsSample(4)], { pen_corrections: [correction] });
  model.update(release);
  assert.equal(model.strokes.length, 1, "correction replaces rather than appends a stroke");
  assert.equal(stroke.completed, true);
  assert.equal(stroke.corrected, true);
  assert.equal(stroke.sourceStrokeId, 7);
  assert.ok(stroke.frame.includes("server-a"));
  assert.deepEqual(stroke.points, correction.points);
  assert.equal(stroke.revision, 1, "equal point counts still invalidate cached geometry");
  const revision = model.revision;
  model.update(release);
  assert.equal(model.revision, revision, "repeated correction history must be idempotent");
  const exported = JSON.parse(model.serialize());
  assert.equal(exported.strokes[0].algorithm, "gaitmap-eskf-rts");
  const restored = new AirPenModel();
  assert.equal(restored.restore(model.serialize()), true);
  assert.deepEqual(restored.strokes, model.strokes);
}

{
  const interruptions = {
    hidden: (model) => model.setVisible(false),
    timeout: (model) => model.expire(),
    history_gap: (model) => model.update(rtsState([rtsSample(4, true)])),
    bad_position: (model) => model.update(rtsState([rtsSample(3, true, [NaN, 0, 0])])),
    wrong_id: (model) => model.update(rtsState([rtsSample(3, true, [0, 0, 0], 8)])),
    training: (model) => model.update(rtsState([rtsSample(3, true)], { training: { state: "recording" } })),
    disconnect: (model) => model.update(rtsState([rtsSample(3, true)], { connection: { state: "disconnected", epoch: 1 } })),
    storage_limit: (model) => model.update(rtsState([rtsSample(3, true)], { pen_motion: { available: true, blocked: true, reason: "stroke_capacity" } })),
  };
  for (const [name, interrupt] of Object.entries(interruptions)) {
    const model = readyRts();
    model.update(rtsState([rtsSample(2, true)]));
    interrupt(model);
    model.setVisible(true);
    const before = structuredClone(model.strokes);
    model.update(rtsState([rtsSample(5)], { pen_corrections: [correction] }));
    model.update(rtsState([rtsSample(6)], { pen_corrections: [correction] }));
    assert.deepEqual(model.strokes, before, `${name}: incomplete ink must never gain unobserved points`);
    assert.equal(model.strokes[0].corrected, false, name);
    assert.equal(model.strokes[0].completed, false, name);
  }
}

{
  const model = readyRts();
  model.update(rtsState([rtsSample(2)], { pen_corrections: [correction] }));
  assert.equal(model.strokes.length, 0, "history alone cannot create a stroke");
  model.update(rtsState([rtsSample(3, true), rtsSample(4)]));
  model.undo();
  model.update(rtsState([rtsSample(5)], { pen_corrections: [correction] }));
  assert.equal(model.strokes.length, 0, "correction cannot revive undone ink");
  assert.equal(model.redoStrokes[0].corrected, false, "correction must not alter the undo stack");
  model.redo();
  model.update(rtsState([rtsSample(6)], { pen_corrections: [correction] }));
  assert.equal(model.strokes[0].corrected, true, "explicit redo permits correction of fully recorded ink");
  model.clear();
  model.update(rtsState([rtsSample(7)], { pen_corrections: [correction] }));
  assert.equal(model.strokes.length, 0, "correction cannot revive cleared ink");
  for (const unavailable of [
    { training: { state: "recording" } },
    { training: { state: "ready" } },
  ]) {
    model.update(rtsState([rtsSample(8, true), rtsSample(9)], { ...unavailable, pen_corrections: [correction] }));
    assert.equal(model.strokes.length, 0, "training must not create corrected ink");
  }
}

{
  const model = readyRts();
  model.update(rtsState([rtsSample(2, true), rtsSample(3)]));
  const oldStroke = structuredClone(model.strokes[0]);
  const restarted = { server_instance: "server-b", pen_corrections: [correction] };
  model.update(rtsState([rtsSample(1)], restarted));
  model.update(rtsState([rtsSample(2)], restarted));
  assert.deepEqual(model.strokes[0], oldStroke, "a restarted server cannot reuse an old stroke id");
  model.update(rtsState([rtsSample(3, true), rtsSample(4)], restarted));
  assert.equal(model.strokes.length, 2);
  assert.deepEqual(model.strokes[0], oldStroke);
  assert.equal(model.strokes[1].corrected, true);
}

{
  const model = readyRts();
  model.update(rtsState([rtsSample(2, true), rtsSample(3)]));
  const before = structuredClone(model.strokes);
  for (const points of [[], [[NaN, 0, 0]], [[0, 0]], Array.from({ length: 40001 }, () => [0, 0, 0])]) {
    model.update(rtsState([], { pen_corrections: [{ stroke_id: 7, points }] }));
    assert.deepEqual(model.strokes, before, "invalid or oversized corrections preserve recorded ink");
  }
  model.update(rtsState([], { pen_corrections: [{ ...correction, stroke_id: -1 }] }));
  assert.deepEqual(model.strokes, before);
  const legacy = JSON.parse(model.serialize());
  for (const s of legacy.strokes) {
    delete s.algorithm;
    delete s.sourceStrokeId;
    delete s.completed;
    delete s.corrected;
  }
  const restored = new AirPenModel();
  assert.equal(restored.restore(JSON.stringify(legacy)), true, "older world-space drafts remain loadable");
  assert.equal(restored.strokes[0].algorithm, undefined, "old drafts are not relabeled as RTS output");
}

console.log(
  "World-space air pen: 15 groups passed (KEY capture, 3D drafts, motion boundaries, RTS replacement, incomplete-stroke rejection, restart identity, correction bounds).",
);
