import * as THREE from "./vendor/three.module.js";
import { OrbitControls } from "./vendor/OrbitControls.js";

const MAX_POINTS = 40000;
const MAX_STROKES = 300;
const STORAGE_KEY = "dm-mc02-air-pen-world-v1";
const TRAJECTORY_ALGORITHM = "gaitmap-eskf-rts";
const REFERENCE_FRAME = {
  axes: "gravity-aligned",
  up_axis: "z",
  heading: "relative-yaw",
  origin: "local-inertial",
};
const MOTION_REASONS = {
  release_required: "等待板上 KEY 松开",
  invalid_sample: "惯性数据异常",
  sample_gap: "惯性数据中断",
  speed_limit: "速度估计超限",
  stroke_limit: "单笔位移超限",
  position_limit: "累计位移超限",
  stroke_capacity: "单笔已达 60 秒存储容量",
};
const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
const finiteVector = (values, size) =>
  Array.isArray(values) &&
  values.length === size &&
  values.every(Number.isFinite);
const validPosition = (values) =>
  finiteVector(values, 3) && values.every((v) => Math.abs(v) <= 1000);
const validSample = (s) =>
  finiteVector(s?.q, 4) &&
  Math.hypot(...s.q) > 0.001 &&
  validPosition(s.p) &&
  Number.isFinite(s.t) &&
  typeof s.key_down === "boolean";
const roundPoint = (p) => p.map((v) => Math.round(v * 1000000) / 1000000);
const validStrokeId = (value) => Number.isSafeInteger(value) && value >= 0;
const scenePoint = (p) => new THREE.Vector3(p[0], p[2], -p[1]);

export class AirPenModel {
  constructor() {
    this.strokes = [];
    this.redoStrokes = [];
    this.cursor = [0, 0, 0];
    this.color = "#252a2c";
    this.width = 3;
    this.sensitivity = 1;
    this.recording = false;
    this.available = false;
    this.reason = "等待连接";
    this.latest = null;
    this.lastSeq = 0;
    this.epoch = null;
    this.revision = 0;
    this.visible = false;
    this.needsSync = true;
    this.released = false;
    this.activeStroke = null;
  }

  get pointCount() {
    return this.strokes.reduce((sum, s) => sum + s.points.length, 0);
  }

  pause(reason = "等待 KEY 松开") {
    this.recording = false;
    this.activeStroke = null;
    this.released = false;
    this.reason = reason;
  }

  setVisible(value) {
    if (this.visible === Boolean(value)) return;
    this.visible = Boolean(value);
    this.needsSync = true;
    this.pause(this.visible ? "等待 KEY 状态" : "页面已隐藏");
  }

  expire() {
    this.available = false;
    this.needsSync = true;
    this.pause("数据中断，等待 KEY 松开");
  }

  preview(sample) {
    this.latest = sample;
    this.cursor = roundPoint(sample.p);
  }

  update(state) {
    const epoch = JSON.stringify([
      state.server_instance || "legacy",
      state.connection?.epoch ?? 0,
      state.pen_epoch ?? 0,
    ]);
    if (epoch !== this.epoch) {
      this.epoch = epoch;
      this.lastSeq = 0;
      this.latest = null;
      this.needsSync = true;
      this.pause("坐标参考已更新，等待 KEY 松开");
    }
    const poseValid = state.pen_motion
      ? state.pen_motion.available === true
      : state.pose?.available === true &&
        (["tracking", "stationary"].includes(state.pose.status) ||
          (state.pose.status === "gap" &&
            state.pose.position_limited === true));
    const connected = state.connection?.state === "connected";
    const keyAvailable = state.key?.available === true;
    const training = ["recording", "ready"].includes(state.training?.state);
    const motionBlocked = state.pen_motion?.blocked === true;
    this.available =
      connected && poseValid && keyAvailable && !training && !motionBlocked;
    const incoming = (
      Array.isArray(state.pen_samples) ? state.pen_samples : []
    ).filter((s) => Number.isSafeInteger(s?.seq) && s.seq > this.lastSeq);
    const tail = incoming.at(-1);
    if (!this.available || !this.visible || this.needsSync) {
      const wasSync = this.needsSync;
      this.pause(
        !connected
          ? "设备未连接"
          : !keyAvailable
            ? state.protocol?.capabilities?.includes("RAW_KEY")
              ? "等待实时 KEY 采样"
              : "固件未提供 KEY 状态，请更新至 r5"
            : training
              ? "动作学习中，画笔暂停"
              : motionBlocked
                ? state.pen_motion.reason === "release_required"
                  ? MOTION_REASONS.release_required
                  : `${MOTION_REASONS[state.pen_motion.reason] || "单笔位移已暂停"}，等待 KEY 松开`
                : !poseValid
                  ? "等待姿态数据"
                  : "等待板上 KEY 按下",
      );
      if (tail && validSample(tail)) this.preview(tail);
      this.lastSeq = Math.max(
        this.lastSeq,
        tail?.seq || 0,
        Number.isSafeInteger(state.pen_cursor) ? state.pen_cursor : 0,
      );
      this.needsSync = !this.available || !this.visible;
      if (this.available && this.visible && wasSync) {
        this.released =
          validSample(tail) &&
          tail.key_down === false &&
          state.key.down === false;
        if (!this.released) this.reason = "等待板上 KEY 松开";
      }
      return this.recording;
    }
    for (const sample of incoming) {
      const previous = this.latest;
      const gap =
        sample.seq !== this.lastSeq + 1 ||
        (previous &&
          !(
            (sample.t - previous.t) >>> 0 > 0 &&
            (sample.t - previous.t) >>> 0 <= 250
          ));
      this.lastSeq = sample.seq;
      if (!validSample(sample)) {
        this.pause("传感器或 KEY 数据异常");
        this.latest = null;
        continue;
      }
      this.preview(sample);
      if (gap) this.pause("数据中断，等待 KEY 松开");
      if (
        this.activeStroke &&
        validStrokeId(this.activeStroke.sourceStrokeId) &&
        sample.stroke_id !== this.activeStroke.sourceStrokeId
      ) {
        this.pause("笔画数据已中断，等待 KEY 松开");
      }
      if (!sample.key_down) {
        if (this.recording && this.activeStroke) {
          this.activeStroke.completed = true;
          this.revision += 1;
        }
        this.recording = false;
        this.activeStroke = null;
        this.released = true;
        this.reason = "等待板上 KEY 按下";
        continue;
      }
      if (!this.recording && this.released) {
        if (
          this.strokes.length >= MAX_STROKES ||
          this.pointCount >= MAX_POINTS
        ) {
          this.pause("笔记已满，请导出后清空");
          continue;
        }
        const stroke = {
          color: this.color,
          width: this.width,
          frame: this.epoch,
          points: [[...this.cursor]],
          completed: false,
          corrected: false,
        };
        if (validStrokeId(sample.stroke_id)) stroke.sourceStrokeId = sample.stroke_id;
        if (state.pen_motion?.algorithm === TRAJECTORY_ALGORITHM) {
          stroke.algorithm = TRAJECTORY_ALGORITHM;
        }
        this.strokes.push(stroke);
        this.activeStroke = stroke;
        this.redoStrokes = [];
        this.recording = true;
        this.released = false;
        this.reason = "KEY 已按下";
        this.revision += 1;
        if (this.pointCount >= MAX_POINTS) this.pause("笔记已满，请导出后清空");
      } else if (this.recording) {
        const stroke = this.activeStroke;
        const last = stroke.points.at(-1);
        if (Math.hypot(...this.cursor.map((v, i) => v - last[i])) >= 0.0003) {
          stroke.points.push([...this.cursor]);
          this.revision += 1;
          if (this.pointCount >= MAX_POINTS)
            this.pause("笔记已满，请导出后清空");
        }
      }
    }
    this.applyCorrections(state.pen_corrections);
    return this.recording;
  }

  applyCorrections(corrections) {
    if (!Array.isArray(corrections)) return;
    for (const correction of corrections) {
      if (!validStrokeId(correction?.stroke_id)) continue;
      const stroke = this.strokes.find((candidate) =>
        candidate.frame === this.epoch &&
        candidate.sourceStrokeId === correction.stroke_id &&
        candidate.completed === true &&
        candidate.corrected !== true,
      );
      if (!stroke || !Array.isArray(correction.points) || !correction.points.length) continue;
      if (this.pointCount - stroke.points.length + correction.points.length > MAX_POINTS) {
        this.reason = "笔记已满，请导出后清空";
        continue;
      }
      if (!correction.points.every(validPosition)) continue;
      // Only a fully observed KEY release authorizes replacement of this stroke.
      stroke.points = correction.points.map(roundPoint);
      stroke.corrected = true;
      stroke.algorithm = TRAJECTORY_ALGORITHM;
      stroke.revision = (stroke.revision || 0) + 1;
      this.revision += 1;
    }
  }

  undo() {
    this.pause();
    if (this.strokes.length) {
      this.redoStrokes.push(this.strokes.pop());
      this.revision += 1;
    }
  }
  redo() {
    this.pause();
    if (this.redoStrokes.length) {
      this.strokes.push(this.redoStrokes.pop());
      this.revision += 1;
    }
  }
  clear() {
    this.pause();
    this.strokes = [];
    this.redoStrokes = [];
    this.revision += 1;
  }
  serialize() {
    return JSON.stringify({
      version: 4,
      dimensions: 3,
      units: "m",
      reference_frame: REFERENCE_FRAME,
      strokes: this.strokes,
      color: this.color,
      width: this.width,
      sensitivity: this.sensitivity,
    });
  }
  restore(text) {
    try {
      const data = JSON.parse(text);
      if (
        data.version !== 4 ||
        data.dimensions !== 3 ||
        data.units !== "m" ||
        Object.entries(REFERENCE_FRAME).some(
          ([key, value]) => data.reference_frame?.[key] !== value,
        ) ||
        !Array.isArray(data.strokes) ||
        data.strokes.length > MAX_STROKES
      )
        return false;
      let total = 0;
      for (const s of data.strokes) {
        if (
          !/^#[0-9a-f]{6}$/i.test(s.color) ||
          !Number.isFinite(s.width) ||
          s.width < 1 ||
          s.width > 12 ||
          typeof s.frame !== "string" ||
          s.frame.length > 100 ||
          (s.sourceStrokeId !== undefined && !validStrokeId(s.sourceStrokeId)) ||
          (s.completed !== undefined && typeof s.completed !== "boolean") ||
          (s.corrected !== undefined && typeof s.corrected !== "boolean") ||
          (s.corrected === true && s.completed !== true) ||
          (s.algorithm !== undefined && (typeof s.algorithm !== "string" || s.algorithm.length > 80)) ||
          (s.revision !== undefined && (!Number.isSafeInteger(s.revision) || s.revision < 0)) ||
          !Array.isArray(s.points) ||
          !s.points.length ||
          !s.points.every(validPosition)
        )
          return false;
        total += s.points.length;
        if (total > MAX_POINTS) return false;
      }
      this.pause();
      this.strokes = data.strokes;
      this.redoStrokes = [];
      this.color = /^#[0-9a-f]{6}$/i.test(data.color) ? data.color : this.color;
      this.width = Number.isFinite(data.width) ? clamp(data.width, 1, 12) : 3;
      this.sensitivity = Number.isFinite(data.sensitivity)
        ? clamp(data.sensitivity, 0.5, 3)
        : 1;
      this.revision += 1;
      return true;
    } catch {
      return false;
    }
  }
}

export function createAirPen(root, options = {}) {
  const $ = (selector) => root.querySelector(selector);
  const canvas = $("#air-pen-canvas");
  const model = new AirPenModel();
  const notify = options.notify || (() => {});
  const renderer = new THREE.WebGLRenderer({
    canvas,
    antialias: true,
    preserveDrawingBuffer: true,
  });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0xf6f8fa);
  const camera = new THREE.PerspectiveCamera(38, 1, 0.001, 2000);
  camera.position.set(0.62, 0.42, 0.55);
  const controls = new OrbitControls(camera, canvas);
  controls.enableDamping = true;
  controls.dampingFactor = 0.15;
  controls.minDistance = 0.02;
  controls.maxDistance = 1000;
  controls.target.set(0.1, 0, 0);
  controls.update();
  const guides = new THREE.Group();
  const grid = new THREE.GridHelper(2, 40, 0xb5c5cd, 0xe0e6ea);
  grid.position.y = -0.002;
  guides.add(grid);
  const axes = [
    new THREE.Vector3(1, 0, 0),
    new THREE.Vector3(0, 0, -1),
    new THREE.Vector3(0, 1, 0),
  ];
  axes.forEach((axis, i) =>
    guides.add(
      new THREE.ArrowHelper(
        axis,
        new THREE.Vector3(),
        0.24,
        [0xc96353, 0x189c78, 0x477fd4][i],
        0.02,
        0.01,
      ),
    ),
  );
  scene.add(guides);
  const ink = new THREE.Group();
  scene.add(ink);
  const pointer = new THREE.Group();
  const pointerMaterial = new THREE.MeshBasicMaterial({ color: 0x0b8f91 });
  const pointerBall = new THREE.Mesh(
    new THREE.SphereGeometry(0.004, 12, 8),
    pointerMaterial,
  );
  pointer.add(pointerBall);
  scene.add(pointer);
  const cylinderGeometry = new THREE.CylinderGeometry(1, 1, 1, 6);
  const jointGeometry = new THREE.SphereGeometry(1, 6, 4);
  const strokeMeshes = new Map();
  const transform = new THREE.Object3D();
  const up = new THREE.Vector3(0, 1, 0);
  let visible = false;
  let disposed = false;
  let lastSampleTime = 0;
  let lastSaved = -1;
  let saveTimer = null;
  let storageFailed = false;
  let lastState = null;
  let framedEpoch = null;
  let demoHeld = false;
  const listeners = [];
  const listen = (element, event, callback) => {
    if (!element) return;
    element.addEventListener(event, callback);
    listeners.push(() => element.removeEventListener(event, callback));
  };
  const text = (id, value) => {
    const element = $(`#${id}`);
    if (element) element.textContent = value;
  };

  function save() {
    clearTimeout(saveTimer);
    saveTimer = null;
    try {
      localStorage.setItem(STORAGE_KEY, model.serialize());
      lastSaved = model.revision;
      storageFailed = false;
      text("pen-draft-status", "3D 草稿已保存");
    } catch {
      text("pen-draft-status", "草稿未保存");
      if (!storageFailed)
        notify("本地存储不可用，请导出 3D 数据保存笔记", "error");
      storageFailed = true;
    }
  }
  function scheduleSave() {
    if (lastSaved === model.revision || saveTimer) return;
    text("pen-draft-status", "保存中");
    saveTimer = setTimeout(save, 600);
  }
  function removeMesh(entry) {
    ink.remove(entry.lines, entry.joints);
    entry.lines.dispose();
    entry.joints.dispose();
    entry.material.dispose();
  }
  function syncInk() {
    const current = new Set(model.strokes);
    for (const [stroke, entry] of strokeMeshes)
      if (!current.has(stroke)) {
        removeMesh(entry);
        strokeMeshes.delete(stroke);
      }
    for (const stroke of model.strokes) {
      let entry = strokeMeshes.get(stroke);
      if (
        entry &&
        entry.count === stroke.points.length &&
        entry.revision === (stroke.revision || 0)
      )
        continue;
      if (!entry || entry.capacity < stroke.points.length) {
        if (entry) removeMesh(entry);
        const capacity =
          2 ** Math.ceil(Math.log2(Math.max(32, stroke.points.length)));
        const material = new THREE.MeshBasicMaterial({ color: stroke.color });
        const lines = new THREE.InstancedMesh(
          cylinderGeometry,
          material,
          capacity,
        );
        const joints = new THREE.InstancedMesh(
          jointGeometry,
          material,
          capacity,
        );
        lines.frustumCulled = joints.frustumCulled = false;
        entry = {
          lines,
          joints,
          material,
          capacity,
          count: 0,
          revision: stroke.revision || 0,
        };
        strokeMeshes.set(stroke, entry);
        ink.add(lines, joints);
      }
      const radius = stroke.width * 0.0005;
      const firstChanged =
        entry.revision === (stroke.revision || 0) ? entry.count : 0;
      for (let i = firstChanged; i < stroke.points.length; i += 1) {
        const p = scenePoint(stroke.points[i]);
        transform.position.copy(p);
        transform.quaternion.identity();
        transform.scale.setScalar(radius);
        transform.updateMatrix();
        entry.joints.setMatrixAt(i, transform.matrix);
        if (i) {
          const previous = scenePoint(stroke.points[i - 1]);
          const direction = p.clone().sub(previous);
          transform.position.copy(p).add(previous).multiplyScalar(0.5);
          transform.quaternion.setFromUnitVectors(
            up,
            direction.clone().normalize(),
          );
          transform.scale.set(radius, direction.length(), radius);
          transform.updateMatrix();
          entry.lines.setMatrixAt(i - 1, transform.matrix);
        }
      }
      entry.count = stroke.points.length;
      entry.revision = stroke.revision || 0;
      entry.joints.count = entry.count;
      entry.lines.count = Math.max(0, entry.count - 1);
      entry.lines.instanceMatrix.needsUpdate =
        entry.joints.instanceMatrix.needsUpdate = true;
    }
  }
  function fitCamera() {
    const box = new THREE.Box3();
    for (const stroke of model.strokes)
      for (const p of stroke.points) box.expandByPoint(scenePoint(p));
    if (box.isEmpty()) box.expandByPoint(scenePoint(model.cursor));
    const center = box.getCenter(new THREE.Vector3());
    const size = box.getSize(new THREE.Vector3()).length();
    const distance = Math.max(
      0.45,
      (size /
        (2 * Math.tan(THREE.MathUtils.degToRad(camera.getEffectiveFOV()) / 2)) /
        Math.min(camera.aspect, 1)) *
        1.35,
    );
    const direction = camera.position.clone().sub(controls.target).normalize();
    controls.target.copy(center);
    camera.position.copy(center).addScaledVector(direction, distance);
    controls.update();
  }
  function resize() {
    if (!visible || disposed) return;
    const rect = canvas.parentElement.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    renderer.setSize(Math.round(rect.width), Math.round(rect.height), false);
    camera.aspect = rect.width / rect.height;
    camera.updateProjectionMatrix();
  }
  function renderUI() {
    if (disposed) return;
    text("pen-status", model.recording ? "记录中" : "待机");
    text("pen-detail", model.reason);
    text(
      "pen-stroke-count",
      `${model.strokes.length} 笔 · ${model.pointCount} 点`,
    );
    text(
      "pen-position-detail",
      lastState?.pen_motion?.blocked
        ? MOTION_REASONS[lastState.pen_motion.reason] || "单笔位移已暂停"
        : "世界参考坐标 · 相对位移",
    );
    for (const id of ["pen-undo", "pen-clear", "pen-export", "pen-export-json"])
      if ($(`#${id}`)) $(`#${id}`).disabled = !model.strokes.length;
    if ($("#pen-redo")) $("#pen-redo").disabled = !model.redoStrokes.length;
    const demo = $("#pen-demo-key");
    if (demo) {
      demo.hidden = lastState?.connection?.demo !== true;
      demo.disabled = !model.available;
      demo.setAttribute("aria-pressed", String(demoHeld));
    }
    root.classList.toggle("pen-recording", model.recording);
    syncInk();
    pointer.visible = model.available && !!model.latest;
    pointerBall.position.copy(scenePoint(model.cursor));
    pointerMaterial.color.set(model.recording ? model.color : "#0b8f91");
  }
  function stop(reason) {
    model.pause(reason);
    save();
    renderUI();
  }
  function action(callback) {
    return () => {
      callback();
      save();
      renderUI();
    };
  }
  function download(blob, extension) {
    if (!blob) {
      notify("导出失败，请重试", "error");
      return;
    }
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `air-note-3d-${new Date().toISOString().replace(/[:.]/g, "-")}.${extension}`;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  function setDemoKey(down) {
    if (
      !options.setDemoKey ||
      lastState?.connection?.demo !== true ||
      demoHeld === down
    )
      return;
    demoHeld = down;
    Promise.resolve(options.setDemoKey(down)).catch(() => {
      demoHeld = false;
      notify("模拟 KEY 操作失败", "error");
    });
    renderUI();
  }

  try {
    const draft = localStorage.getItem(STORAGE_KEY);
    if (draft && model.restore(draft)) {
      text("pen-draft-status", "3D 草稿已恢复");
      camera.zoom = model.sensitivity;
      camera.updateProjectionMatrix();
      fitCamera();
    } else text("pen-draft-status", "3D 本地草稿");
  } catch {
    text("pen-draft-status", "本地存储不可用");
  }
  lastSaved = model.revision;
  for (const [id, key, suffix] of [
    ["pen-width", "width", " mm"],
    ["pen-sensitivity", "sensitivity", "x"],
  ]) {
    const input = $(`#${id}`);
    if (!input) continue;
    input.value = model[key];
    text(`${id}-value`, `${model[key]}${suffix}`);
    listen(input, "input", () => {
      if (key === "width") model.pause();
      model[key] = clamp(
        Number(input.value) || 1,
        key === "width" ? 1 : 0.5,
        key === "width" ? 12 : 3,
      );
      if (key === "sensitivity") {
        camera.zoom = model.sensitivity;
        camera.updateProjectionMatrix();
      }
      text(`${id}-value`, `${model[key]}${suffix}`);
      model.revision += 1;
      save();
      renderUI();
    });
  }
  const swatches = [...root.querySelectorAll("[data-pen-color]")];
  const colorState = () =>
    swatches.forEach((s) =>
      s.setAttribute(
        "aria-pressed",
        String(s.dataset.penColor === model.color),
      ),
    );
  for (const swatch of swatches)
    listen(
      swatch,
      "click",
      action(() => {
        model.pause();
        model.color = swatch.dataset.penColor;
        model.revision += 1;
        colorState();
      }),
    );
  colorState();
  listen(
    $("#pen-undo"),
    "click",
    action(() => model.undo()),
  );
  listen(
    $("#pen-redo"),
    "click",
    action(() => model.redo()),
  );
  listen($("#pen-center"), "click", fitCamera);
  const dialog = $("#pen-clear-dialog");
  listen($("#pen-clear"), "click", () => {
    model.setVisible(false);
    stop();
    dialog?.showModal();
  });
  listen(dialog, "close", () => {
    model.setVisible(visible && !document.hidden);
    renderUI();
  });
  listen($("#pen-clear-cancel"), "click", () => dialog?.close());
  listen(
    $("#pen-clear-confirm"),
    "click",
    action(() => {
      model.clear();
      dialog?.close();
    }),
  );
  listen($("#pen-export-json"), "click", () =>
    download(
      new Blob([model.serialize()], { type: "application/json" }),
      "json",
    ),
  );
  listen($("#pen-export"), "click", () => {
    const oldSize = renderer.getSize(new THREE.Vector2());
    const oldRatio = renderer.getPixelRatio();
    const oldAspect = camera.aspect;
    const pointerVisible = pointer.visible;
    pointer.visible = guides.visible = false;
    scene.background.set(0xffffff);
    renderer.setPixelRatio(1);
    renderer.setSize(1600, 1000, false);
    camera.aspect = 1.6;
    camera.updateProjectionMatrix();
    renderer.render(scene, camera);
    canvas.toBlob((blob) => download(blob, "png"), "image/png");
    scene.background.set(0xf6f8fa);
    pointer.visible = pointerVisible;
    guides.visible = true;
    renderer.setPixelRatio(oldRatio);
    renderer.setSize(oldSize.x, oldSize.y, false);
    camera.aspect = oldAspect;
    camera.updateProjectionMatrix();
  });
  const demoButton = $("#pen-demo-key");
  listen(demoButton, "pointerdown", (event) => {
    event.preventDefault();
    demoButton.setPointerCapture(event.pointerId);
    setDemoKey(true);
  });
  for (const event of ["pointerup", "pointercancel", "lostpointercapture"])
    listen(demoButton, event, () => setDemoKey(false));
  listen(demoButton, "keydown", (event) => {
    if ([" ", "Enter"].includes(event.key)) {
      event.preventDefault();
      setDemoKey(true);
    }
  });
  listen(demoButton, "keyup", (event) => {
    if ([" ", "Enter"].includes(event.key)) {
      event.preventDefault();
      setDemoKey(false);
    }
  });
  listen(demoButton, "blur", () => setDemoKey(false));
  listen(window, "blur", () => setDemoKey(false));
  listen(document, "visibilitychange", () => {
    model.setVisible(visible && !document.hidden && !dialog?.open);
    if (document.hidden) {
      setDemoKey(false);
      save();
    }
    renderUI();
  });
  listen(window, "pagehide", save);
  const resizeObserver = new ResizeObserver(resize);
  resizeObserver.observe(canvas.parentElement);
  const watchdog = setInterval(() => {
    if (model.available && Date.now() - lastSampleTime > 650) {
      model.expire();
      save();
      renderUI();
    }
  }, 100);
  let frame = 0;
  function animate() {
    if (disposed) return;
    frame = requestAnimationFrame(animate);
    if (!visible || document.hidden) return;
    controls.update();
    renderer.render(scene, camera);
  }
  animate();
  renderUI();
  return {
    update(state) {
      lastState = state;
      const wasRecording = model.recording;
      const previousSeq = model.lastSeq;
      const previousStrokeCount = model.strokes.length;
      model.update(state);
      if (
        (model.latest &&
          !model.strokes.length &&
          framedEpoch !== model.epoch) ||
        (!previousStrokeCount && model.strokes.length)
      ) {
        fitCamera();
        framedEpoch = model.epoch;
      }
      if (model.lastSeq !== previousSeq) lastSampleTime = Date.now();
      if (wasRecording && !model.recording) save();
      else scheduleSave();
      renderUI();
    },
    setVisible(value) {
      if (value && !visible && !model.strokes.length) framedEpoch = null;
      visible = Boolean(value);
      model.setVisible(visible && !document.hidden && !dialog?.open);
      if (!visible) {
        setDemoKey(false);
        save();
      }
      resize();
      renderUI();
    },
    dispose() {
      setDemoKey(false);
      model.pause();
      save();
      disposed = true;
      cancelAnimationFrame(frame);
      clearInterval(watchdog);
      clearTimeout(saveTimer);
      resizeObserver.disconnect();
      listeners.forEach((remove) => remove());
      controls.dispose();
      strokeMeshes.forEach(removeMesh);
      cylinderGeometry.dispose();
      jointGeometry.dispose();
      scene.traverse((object) => {
        object.geometry?.dispose();
        if (Array.isArray(object.material))
          object.material.forEach((m) => m.dispose());
        else object.material?.dispose();
      });
      renderer.dispose();
    },
  };
}
