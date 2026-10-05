"use strict";

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const revisionAttribute = document.currentScript?.dataset.uiRevision || "";
const UI_REVISION = revisionAttribute.includes("__GUI_REVISION__")
  ? ""
  : revisionAttribute;
const TAB_STORAGE_KEY = "gesture-selected-tab";
const TAB_NAMES = new Set(["library", "live", "spatial", "pen", "capture"]);
const moduleUrl = (name) => `/${name}?v=${encodeURIComponent(UI_REVISION)}`;
const ui = {
  token: "",
  state: null,
  cursor: 0,
  events: [],
  selected: 1,
  tab: "library",
  charts: null,
  lastWave: null,
  lastRecognition: null,
  deleteSlot: null,
  polling: false,
  lastPollAt: 0,
  busy: false,
  toastTimer: null,
  serverError: "",
  capturesKey: "",
  demoKeyPressed: false,
  demoKeyQueue: Promise.resolve(),
  spatialView: null,
  spatialLoading: null,
  penView: null,
  penLoading: null,
  trailVisible: true,
  reloading: false,
  serverInstance: "",
};
const slotColors = [
  "#ff0000",
  "#00ff00",
  "#0000ff",
  "#ffff00",
  "#00ffff",
  "#ff00ff",
  "#ff8000",
  "#ffffff",
];
const slotColorNames = ["红", "绿", "蓝", "黄", "青", "品红", "橙", "白"];
const blankSlots = () =>
  Array.from({ length: 8 }, (_, index) => ({
    id: index + 1,
    name: `动作 ${String(index + 1).padStart(2, "0")}`,
    hotkey: "",
    state: "unknown",
    templates: 0,
    color: slotColors[index],
    color_name: slotColorNames[index],
  }));
const icons = () => window.lucide?.createIcons();
const pad = (number) => String(number).padStart(2, "0");
const isConnected = () =>
  !ui.serverError && ui.state?.connection?.state === "connected";
const isConnecting = () =>
  ["connecting", "reconnecting"].includes(ui.state?.connection?.state);
const slotById = (id) =>
  (ui.state?.slots || blankSlots()).find(
    (slot) => Number(slot.id) === Number(id),
  );
const supportsManualTraining = () =>
  ui.state?.protocol?.manual_training === true;
const hasCapability = (name) =>
  (ui.state?.protocol?.capabilities || []).includes(name);
const isDemo = () =>
  Boolean(ui.state?.connection?.demo || ui.state?.connection?.simulation);

function renderSwatch(element, slot) {
  const index = Number(slot?.id) - 1;
  const color = /^#[0-9a-f]{6}$/i.test(slot?.color || "")
    ? slot.color
    : slotColors[index] || "#d7e1dc";
  const name = slot?.color_name || slotColorNames[index] || "未指定";
  element.style.backgroundColor = color;
  element.title = `RGB · ${name}`;
  element.setAttribute("aria-label", `RGB ${name}`);
}
const numberText = (value) =>
  value === null || value === undefined
    ? "—"
    : Number(value).toLocaleString("zh-CN");
const escapeText = (value) =>
  String(value ?? "").replace(
    /[&<>"']/g,
    (character) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        character
      ],
  );

function toast(message, error = false) {
  clearTimeout(ui.toastTimer);
  const element = $("#toast");
  element.textContent = message;
  element.classList.toggle("error", error);
  element.hidden = false;
  ui.toastTimer = setTimeout(
    () => {
      element.hidden = true;
    },
    error ? 6500 : 3500,
  );
}

async function api(path, payload) {
  const options =
    payload === undefined
      ? {}
      : {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            "X-Gesture-Token": ui.token,
          },
          body: JSON.stringify(payload),
        };
  const response = await fetch(path, { ...options, cache: "no-store" });
  let result;
  try {
    result = await response.json();
  } catch {
    throw new Error("本地服务未返回有效响应");
  }
  if (checkServerVersion(result.state || result)) {
    throw new Error("本地服务已更新，正在刷新页面");
  }
  if (!response.ok || result.ok === false)
    throw new Error(result.error || `请求失败 (${response.status})`);
  if (result.state) applyState(result.state);
  return result;
}

function checkServerVersion(result) {
  if (ui.reloading) return true;
  const revision = result?.ui_revision || "";
  const instance = result?.server_instance || "";
  const changed =
    (UI_REVISION && revision && revision !== UI_REVISION) ||
    (ui.serverInstance && instance && instance !== ui.serverInstance);
  if (!ui.serverInstance && instance) ui.serverInstance = instance;
  if (!changed) return false;
  ui.reloading = true;
  ui.penView?.setVisible(false);
  ui.spatialView?.setVisible(false);
  const target = `${revision}:${instance}`;
  let previouslyReloaded = false;
  try {
    previouslyReloaded =
      sessionStorage.getItem("gesture-reload-target") === target;
    sessionStorage.setItem("gesture-reload-target", target);
  } catch {}
  if (previouslyReloaded) {
    ui.serverError = "页面版本未能同步，请刷新页面";
    renderState();
    return true;
  }
  window.location.reload();
  return true;
}

async function perform(action) {
  if (ui.busy) return;
  ui.busy = true;
  renderState();
  try {
    return await action();
  } catch (error) {
    toast(error.message, true);
  } finally {
    ui.busy = false;
    renderState();
  }
}

async function command(value) {
  return api("/api/command", { command: value });
}

async function refreshPorts() {
  const select = $("#port-select");
  const oldPort = select.value || ui.state?.connection?.port;
  try {
    const { ports = [] } = await api("/api/ports");
    select.replaceChildren();
    if (!ports.length) select.add(new Option("未发现串口", ""));
    for (const port of ports) {
      const option = new Option(port.device, port.device);
      option.title = port.description || port.device;
      option.dataset.description = port.description || port.device;
      select.add(option);
    }
    if (ports.some((port) => port.device === oldPort)) select.value = oldPort;
    else {
      const preferred =
        ports.find((port) => /0483.*5740/i.test(port.hwid || "")) ||
        ports.find((port) =>
          /STM|STMicroelectronics/i.test(port.description || ""),
        );
      if (preferred) select.value = preferred.device;
    }
    renderState();
  } catch (error) {
    toast(error.message, true);
  }
}

function createSlotRows() {
  $("#slot-rows").innerHTML = blankSlots()
    .map(
      (slot) => `
    <tr data-slot="${slot.id}" class="${slot.id === 1 ? "selected" : ""}">
      <td>${pad(slot.id)}</td>
      <td><div class="slot-name"><span class="color-swatch" role="img"></span><input class="name-input" value="${escapeText(slot.name)}" maxlength="40" aria-label="槽位 ${slot.id} 动作名称"></div></td>
      <td><div class="slot-state"><span class="mini-dot"></span><span class="slot-state-text">未同步</span></div></td>
      <td><input class="hotkey-input" placeholder="未映射" maxlength="80" aria-label="槽位 ${slot.id} 电脑快捷键" spellcheck="false" autocomplete="off"></td>
      <td><div class="slot-actions"><button class="icon-button learn-slot" title="学习动作 ${slot.id}" aria-label="学习动作 ${slot.id}" disabled><i data-lucide="circle-dot"></i></button><button class="icon-button delete-button" title="删除动作 ${slot.id}" aria-label="删除动作 ${slot.id}" disabled><i data-lucide="trash-2"></i></button></div></td>
    </tr>`,
    )
    .join("");
  for (const row of $$("#slot-rows tr")) {
    const slot = Number(row.dataset.slot);
    row.addEventListener("click", () => {
      ui.selected = slot;
      renderState();
    });
    for (const selector of [".name-input", ".hotkey-input"]) {
      const input = row.querySelector(selector);
      input.addEventListener("keydown", (event) => {
        if (event.key === "Enter") input.blur();
      });
      input.addEventListener("change", async () => {
        try {
          await api("/api/settings", {
            slot,
            name: row.querySelector(".name-input").value.trim(),
            hotkey: row.querySelector(".hotkey-input").value.trim(),
          });
        } catch (error) {
          toast(error.message, true);
        }
        renderState();
      });
    }
    row
      .querySelector(".learn-slot")
      .addEventListener("click", () => learnSlot(slot));
    row
      .querySelector(".delete-button")
      .addEventListener("click", () => requestDelete(slot));
  }
  icons();
}

function applyState(state) {
  if (!state || typeof state !== "object") return;
  const hadState = ui.state !== null;
  if (Number(state.cursor) < ui.cursor) ui.cursor = 0;
  if (ui.state && state.connection?.epoch !== ui.state.connection?.epoch) {
    ui.lastRecognition = null;
    ui.lastWave = null;
  }
  ui.state = state;
  ui.serverError = "";
  const freshEvents = (state.events || []).filter(
    (event) => Number(event.seq) > ui.cursor,
  );
  for (const event of freshEvents) {
    ui.events.push(event);
    const kind = String(event.kind || "").toLowerCase();
    if (["event", "recognized", "recognition"].includes(kind)) {
      ui.lastRecognition = event;
    }
    if (hadState && kind === "calibration") {
      const ok = String(event.message).trim().toUpperCase() === "OK";
      toast(ok ? "陀螺仪校准完成" : "校准失败，请保持板子静止后重试", !ok);
    }
    if (hadState && kind === "error")
      toast(errorMessage(event.message || "设备未完成操作"), true);
    if (hadState && kind === "train" && /REJECT/i.test(event.message || ""))
      toast(trainingMessage(event.message), true);
  }
  ui.events = ui.events.slice(-50);
  ui.cursor = Math.max(
    ui.cursor,
    Number(state.cursor) || 0,
    ...freshEvents.map((event) => Number(event.seq) || 0),
  );
  if (
    ["recording", "ready"].includes(state.training?.state) &&
    state.training.slot
  )
    ui.selected = Number(state.training.slot);
  if (
    ui.demoKeyPressed &&
    (!isConnected() ||
      !isDemo() ||
      !["recording", "ready"].includes(state.training?.state))
  )
    releaseDemoKey();
  renderState();
  if (freshEvents.length) renderEvents();
  if (ui.tab === "live") renderCharts();
  const captureKey = `${state.capture?.active}:${state.capture?.id}`;
  if (ui.capturesKey !== captureKey) {
    ui.capturesKey = captureKey;
    refreshCaptures();
  }
}

function renderState() {
  const state = ui.state || {};
  const connected = isConnected();
  const connecting = isConnecting();
  const reconnecting = state.connection?.state === "reconnecting";
  const demo = Boolean(state.connection?.demo || state.connection?.simulation);
  const status = state.status || {};
  const training = state.training || { state: "idle" };
  const trainingActive = ["recording", "ready"].includes(training.state);
  const pending =
    state.pending_delete !== null && state.pending_delete !== undefined;
  $("#connection-dot").classList.toggle("connected", connected);
  const badge = $("#connection-badge");
  badge.className = `badge ${connected ? "connected" : "neutral"}`;
  badge.innerHTML = `<span class="mini-dot ${connected ? "good" : ""}"></span>${connected ? "已连接" : reconnecting ? "重连中" : connecting ? "连接中" : "未连接"}`;
  $("#demo-badge").hidden = !demo;
  $("#connect-button span").textContent = connected
    ? "断开连接"
    : reconnecting
      ? "取消重连"
      : connecting
        ? "取消连接"
        : "连接设备";
  $("#connect-button").disabled =
    ui.busy || (!connected && !connecting && !$("#port-select").value);
  $("#port-select").disabled = connected || connecting || ui.busy;
  $("#refresh-ports").disabled = ui.busy;
  $("#demo-button span").textContent =
    connected && demo ? "退出演示" : "演示设备";
  $("#demo-button").disabled = ui.busy || connecting || (connected && !demo);
  $("#port-description").textContent = connected
    ? demo
      ? "演示设备 · 模拟输入"
      : `${state.connection?.port || "USB CDC"} · 已连接`
    : reconnecting
      ? `${state.connection?.port || "USB CDC"} · 重连 ${state.connection?.retry_attempt || 0}/${state.connection?.retry_limit || 0}`
      : connecting
        ? `${state.connection?.port || "USB CDC"} · 连接中`
        : $("#port-select").selectedOptions[0]?.dataset.description || "未连接设备";
  $("#armed-toggle").checked = connected && Boolean(status.armed);
  $("#armed-toggle").disabled =
    !connected ||
    ui.busy ||
    trainingActive ||
    pending ||
    (!status.armed && !status.classes);
  $("#hotkeys-toggle").checked = Boolean(state.hotkeys_enabled);
  $("#hotkeys-toggle").disabled = !connected || ui.busy || demo;
  $("#hotkeys-toggle").title = demo
    ? "演示设备不发送电脑快捷键"
    : "快捷键作用于当前前台应用";
  $("#calibrate-button").disabled =
    !connected || ui.busy || trainingActive || pending;
  $("#status-button").disabled = !connected || ui.busy;
  $("#stop-button").disabled = !connected;
  $("#calibration-text").textContent = !connected
    ? "等待连接"
    : status.calibrated
      ? "陀螺仪已校准"
      : hasCapability("NO_CALIBRATION")
        ? "可选校准 · 不影响录制"
        : "未校准 · 固件能力待核验";
  $("#calibration-dot").classList.toggle(
    "good",
    connected && Boolean(status.calibrated),
  );
  const error = ui.serverError || state.connection?.error || "";
  $("#connection-error").textContent = error;
  $("#connection-error").hidden = !error;
  const firmware = state.protocol?.firmware;
  $("#firmware-version").textContent = connected
    ? `固件：${firmware || "未返回版本"}`
    : "固件版本未读取";
  const missingCapabilities = [
    "NO_CALIBRATION",
    "RANDOM_START",
    "LIVE_MATCH",
    "ONE_DEMO",
    "SIMILARITY_WARNING",
  ].filter((capability) => !hasCapability(capability));
  const firmwareWarning =
    connected &&
    !demo &&
    (!firmware ||
      missingCapabilities.length ||
      state.protocol?.requires_calibration);
  $("#firmware-warning").hidden = !firmwareWarning;
  $("#firmware-warning").textContent = state.protocol?.requires_calibration
    ? "板端仍返回“需要校准”，运行的是旧流程。请烧录 gesture-20261005-r4 固件并重新连接。"
    : firmware
      ? `当前固件 ${firmware} 未报告完整的新录制能力。请烧录 gesture-20261005-r4 固件并重新连接。`
      : "板端未返回固件版本，尚不能确认支持随机起始姿态和单次录制。请烧录 gesture-20261005-r4 固件并重新连接。";
  $("#metric-classes").innerHTML =
    `${connected ? numberText(status.classes) : "—"}<small> / 8</small>`;
  $("#metric-armed").textContent = !connected
    ? "等待连接"
    : trainingActive
      ? "正在学习"
      : status.armed
        ? "识别已启用"
        : "识别已关闭";
  const recognizedSlot =
    ui.lastRecognition?.id ??
    ui.lastRecognition?.class_id ??
    ui.lastRecognition?.slot;
  $("#last-name").textContent =
    !connected || !ui.lastRecognition
      ? "—"
      : slotById(recognizedSlot)?.name ||
        (recognizedSlot ? `动作 ${pad(recognizedSlot)}` : "已识别动作");
  $("#last-color").hidden = !connected || !recognizedSlot;
  if (recognizedSlot) renderSwatch($("#last-color"), slotById(recognizedSlot));
  $("#metric-latency").innerHTML =
    `${connected ? numberText(status.max_feed_us) : "—"}<small> μs</small>`;
  const liveMatch = state.live_match || {};
  const visibleMatch = liveMatch.id ? liveMatch : state.recent_match || {};
  const candidate =
    connected && status.armed && hasCapability("LIVE_MATCH")
      ? Number(visibleMatch.id) || 0
      : 0;
  $("#live-match-label").textContent =
    candidate && !liveMatch.id ? "刚刚匹配 · RGB" : "运动中匹配 · RGB";
  $("#live-match-color").hidden = !candidate;
  if (candidate) renderSwatch($("#live-match-color"), slotById(candidate));
  $("#live-match-name").textContent = !connected
    ? "等待连接"
    : !hasCapability("LIVE_MATCH")
      ? "需更新固件"
      : !status.armed
        ? "识别已关闭"
        : candidate
          ? slotById(candidate)?.name || `动作 ${pad(candidate)}`
          : "未匹配";
  $("#live-match-distance").textContent =
    candidate && Number.isFinite(visibleMatch.distance)
      ? `距离 ${visibleMatch.distance.toFixed(3)}`
      : "";
  const slots = state.slots || blankSlots();
  const unknownSlots = slots.some((slot) => slot.state === "unknown");
  $("#library-note").textContent = !connected
    ? "等待设备同步"
    : unknownSlots
      ? "固件未提供槽位状态"
      : `${status.classes || 0} 个动作已保存`;
  for (const row of $$("#slot-rows tr")) {
    const slot =
      slots.find((item) => Number(item.id) === Number(row.dataset.slot)) ||
      blankSlots()[Number(row.dataset.slot) - 1];
    row.classList.toggle("selected", Number(slot.id) === ui.selected);
    renderSwatch(row.querySelector(".color-swatch"), slot);
    const nameInput = row.querySelector(".name-input");
    const hotkeyInput = row.querySelector(".hotkey-input");
    if (document.activeElement !== nameInput)
      nameInput.value = slot.name || `动作 ${pad(slot.id)}`;
    if (document.activeElement !== hotkeyInput)
      hotkeyInput.value = slot.hotkey || "";
    const stateElement = row.querySelector(".slot-state");
    stateElement.className = `slot-state ${slot.state === "saved" ? "saved" : ""}`;
    stateElement
      .querySelector(".mini-dot")
      .classList.toggle("good", slot.state === "saved");
    stateElement.querySelector(".slot-state-text").textContent =
      slot.state === "saved"
        ? `${slot.templates || 1} 段 · 已保存`
        : slot.state === "empty"
          ? "空槽位"
          : "未同步";
    row.querySelector(".learn-slot").disabled =
      !connected ||
      ui.busy ||
      trainingActive ||
      pending ||
      !supportsManualTraining() ||
      slot.state !== "empty";
    row.querySelector(".delete-button").disabled =
      !connected ||
      ui.busy ||
      trainingActive ||
      pending ||
      slot.state !== "saved";
  }
  renderTraining();
  $("#delete-confirm").disabled =
    ui.busy || Number(state.pending_delete) !== ui.deleteSlot || !connected;
  $("#delete-wait").textContent =
    Number(state.pending_delete) === ui.deleteSlot
      ? "删除后需要重新示范此动作。"
      : "等待板端确认...";
  for (const [id, key] of [
    ["samples", "samples"],
    ["sample-drops", "sample_drops"],
    ["usb-drops", "usb_drops"],
    ["imu-errors", "imu_errors"],
    ["unknown", "unknown"],
  ])
    $("#stat-" + id).textContent = connected ? numberText(status[key]) : "—";
  const wave = state.waveform || [];
  $("#stream-status").textContent =
    connected && wave.length ? `${demo ? "演示 · " : ""}实时接收` : "等待数据";
  $("#stream-dot").classList.toggle("good", connected && wave.length > 0);
  const capture = state.capture || {};
  $("#capture-badge").textContent = capture.active ? "正在采集" : "未采集";
  $("#capture-badge").className =
    `badge ${capture.active ? "recording" : "neutral"}`;
  $("#capture-button span").textContent = capture.active
    ? "停止采集"
    : "开始采集";
  $("#capture-button").disabled = ui.busy || (!connected && !capture.active);
  for (const input of $$("#capture-form input, #capture-form select"))
    input.disabled = Boolean(capture.active);
  $("#capture-current").hidden = !capture.active;
  $("#capture-current-text").textContent =
    `${capture.id || "当前采集"} · ${numberText(capture.raw_count || 0)} 个样本 · ${numberText(capture.event_count || 0)} 个事件`;
  $("#protocol-label").textContent = state.protocol?.version
    ? `USB · CDC · v${state.protocol.version}`
    : "USB · CDC";
  $("#footer-status").textContent = connected
    ? `${demo ? "演示设备" : state.connection?.port || "USB CDC"} · ${numberText(state.counters?.events || 0)} 次识别 · ${numberText(state.counters?.unknown || 0)} 次拒绝`
    : "等待设备";
  renderSpatial();
  renderPen();
}

function renderTraining() {
  const slot = slotById(ui.selected) || blankSlots()[0];
  const training = ui.state?.training || { state: "idle" };
  const connected = isConnected();
  const manualTraining = supportsManualTraining();
  const active = ["recording", "ready"].includes(training.state);
  const selectedActive = active && Number(training.slot) === ui.selected;
  const ready = selectedActive && training.state === "ready";
  const oneDemo = hasCapability("ONE_DEMO");
  const capturing = selectedActive && Boolean(training.capturing);
  const count = selectedActive
    ? Math.min(3, Number(training.collected) || 0)
    : slot.state === "saved"
      ? slot.templates || 3
      : 0;
  $("#selected-slot").textContent = pad(ui.selected);
  $("#training-title").textContent = slot.name || `动作 ${pad(ui.selected)}`;
  const status = $("#training-status");
  status.className = `training-status ${capturing ? "recording" : ready ? "ready" : selectedActive ? "recording" : connected && !manualTraining ? "attention" : ""}`;
  status.querySelector("span").textContent = !connected
    ? "等待连接设备"
    : !manualTraining
      ? "需更新 KEY 录制固件"
      : capturing
        ? "正在录制，松开 KEY 结束"
        : ready
          ? "可保存动作"
          : selectedActive
            ? "等待按下 KEY"
            : slot.state === "saved"
              ? "已保存到板端"
              : slot.state === "unknown"
                ? "槽位状态未同步"
                : "等待准备学习";
  $("#training-count").textContent = String(count);
  $("#training-count-label").textContent = oneDemo
    ? " 段示范 · 最多 3 段"
    : " / 3 次示范";
  $("#training-template-count").textContent =
    slot.state === "unknown" ? "未同步" : `${slot.templates || 0} 个`;
  $("#training-calibration").textContent = !connected
    ? "未连接"
    : ui.state?.status?.calibrated
      ? "已校准"
      : hasCapability("NO_CALIBRATION")
        ? "未校准（可选）"
        : "未校准（旧固件）";
  $$(".sample-step").forEach((element, index) => {
    element.querySelector("small").textContent = oneDemo
      ? ["示范", "补充（可选）", "补充（可选）"][index]
      : ["示范一", "示范二", "示范三"][index];
    element.classList.toggle("complete", index < count);
    element.classList.toggle(
      "current",
      selectedActive && (!ready || oneDemo) && index === count,
    );
  });
  $("#training-message").textContent = !connected
    ? "未开始录制"
    : !manualTraining
      ? "当前固件不支持按住 KEY 录制"
      : selectedActive
        ? (training.message ? trainingMessage(training.message) : "") ||
          (capturing
            ? `正在采集第 ${count + 1} 段示范`
            : ready
              ? oneDemo && count < 3
                ? "已可保存，也可继续补充示范"
                : "示范已完成，可以保存"
              : `第 ${count + 1} 次示范尚未开始`)
        : slot.state === "saved"
          ? "动作已可用于识别"
          : slot.state === "unknown"
            ? "等待固件返回槽位状态"
            : "未开始录制";
  $("#learn-button span").textContent = "准备学习";
  const warning = selectedActive && !capturing ? training.warning : null;
  $("#training-warning").hidden = !warning;
  $("#training-warning").textContent = warning
    ? similarityMessage(warning)
    : "";
  $("#learn-button").disabled =
    !connected ||
    ui.busy ||
    active ||
    ui.state?.pending_delete != null ||
    slot.state !== "empty" ||
    !manualTraining;
  const demoButton = $("#demo-key-button");
  demoButton.hidden = !connected || !isDemo();
  demoButton.disabled =
    !connected ||
    !isDemo() ||
    !manualTraining ||
    !selectedActive ||
    (ready && (!oneDemo || count >= 3)) ||
    ui.busy;
  demoButton.classList.toggle("held", ui.demoKeyPressed || capturing);
  demoButton.setAttribute(
    "aria-pressed",
    String(ui.demoKeyPressed || capturing),
  );
  demoButton.querySelector("span").textContent =
    ui.demoKeyPressed || capturing ? "松开结束 · 演示" : "按住录制 · 演示";
  $("#save-button").disabled =
    !connected || ui.busy || !ready || capturing || ui.demoKeyPressed;
  $("#cancel-button").disabled = !connected || ui.busy || !active;
}

function similarityMessage(warning) {
  return `示范已录入，可保存。与“${slotById(warning.slot)?.name || `动作 ${pad(warning.slot)}`}”相似，识别可能不确定。`;
}

function trainingMessage(message) {
  const reasons = {
    INCONSISTENT: "动作差异较大，本次示范未计入",
    CONFLICT: hasCapability("SIMILARITY_WARNING")
      ? "槽位已被占用，请选择空槽位"
      : "当前固件拒绝了相似动作；r4 固件可保留示范并提示相似槽位",
    "TOO SHORT": "录制时间过短，本次示范未计入",
    "TOO LONG": "录制时间过长，本次示范未计入",
    QUALITY: "有效运动不足或动作质量不合格，本次示范未计入",
    "LOW QUALITY": "有效运动不足或动作质量不合格，本次示范未计入",
    "SAMPLE GAP": "采样发生中断，本次示范未计入",
    "NOT READY": hasCapability("RANDOM_START")
      ? "板端未收到有效采样，本次示范未计入"
      : "旧固件仍要求起始静止，请烧录 gesture-20261005-r4 后重试",
  };
  const fields = String(message).toUpperCase().split(",");
  for (const field of fields.reverse()) {
    const reason = field.trim().replace(/[_\s-]+/g, " ");
    if (reasons[reason]) return reasons[reason];
  }
  return String(message);
}

function errorMessage(message) {
  const fields = String(message)
    .split(",")
    .map((field) => field.trim());
  const reasons = {
    NOT_CALIBRATED: "板端仍要求校准，请烧录 gesture-20261005-r4 固件并重新连接",
    CALIBRATION_REQUIRED:
      "板端仍要求校准，请烧录 gesture-20261005-r4 固件并重新连接",
    CONFLICT: "槽位已被占用，请选择空槽位",
    NOT_STILL: "板子未保持静止，请放稳后重新校准",
    IMU_UNAVAILABLE: "IMU 未就绪，请检查板端状态",
    TIMER_FAILED: "板端采样定时器启动失败",
    FLASH_UNAVAILABLE: "板载 Flash 未就绪，无法保存动作",
    BUSY: "板端正在执行其他操作",
    NO_GESTURES: "尚无已保存动作",
    NO_CLASSES: "尚无已保存动作",
    NOT_READY: "示范尚未完成，暂不能保存",
    THREE_DEMOS_REQUIRED: "需要完成三次示范后才能保存",
    BAD_COMMAND: "当前固件不支持此操作",
  };
  return reasons[fields.at(-1)] || `设备错误：${message}`;
}

function renderSpatial() {
  const pose = ui.state?.pose || {};
  const available = isConnected() && pose.available === true;
  const value = (number, digits) =>
    available && Number.isFinite(Number(number))
      ? Number(number).toFixed(digits)
      : "—";
  for (const [index, axis] of ["x", "y", "z"].entries()) {
    $("#pose-" + axis).textContent = value(pose.position_m?.[index], 3);
  }
  for (const axis of ["roll", "pitch", "yaw"]) {
    $("#pose-" + axis).textContent = value(pose.euler_deg?.[axis], 1);
  }
  const status = $("#pose-status");
  status.textContent =
    !available || pose.status === "waiting"
      ? "等待 IMU 数据"
      : pose.status === "gap"
        ? "采样中断"
        : pose.stationary
          ? "静止"
          : "运动中";
  status.className = `badge ${!available ? "neutral" : pose.status === "gap" ? "demo" : "connected"}`;
  $("#pose-origin-button").disabled = !available || ui.busy;
  $("#pose-camera-button").disabled = !ui.spatialView;
  $("#pose-trail-button").disabled = !ui.spatialView;
  $("#spatial-waiting").hidden = available || !ui.spatialView;
  $("#pose-range-warning").hidden = !available || !pose.position_limited;
  const speed = Math.hypot(...(pose.velocity_m_s || [0, 0, 0]));
  $("#pose-motion-detail").textContent = available
    ? `速度 ${Number.isFinite(speed) ? speed.toFixed(3) : "—"} m/s · ${Number(pose.elapsed_s || 0).toFixed(1)} s`
    : "未接收姿态数据";
  ui.spatialView?.update(
    available ? pose : { available: false, status: "waiting" },
  );
}

async function ensureSpatial() {
  if (ui.spatialView) {
    ui.spatialView.setVisible(!ui.reloading && ui.tab === "spatial");
    return;
  }
  if (ui.spatialLoading) return ui.spatialLoading;
  const message = $("#spatial-loading");
  message.hidden = false;
  message.textContent = "正在加载三维视图...";
  ui.spatialLoading = import(moduleUrl("spatial.js"))
    .then(({ createSpatialView }) => {
      ui.spatialView = createSpatialView($("#spatial-scene"));
      ui.spatialView.setTrailVisible(ui.trailVisible);
      ui.spatialView.setVisible(!ui.reloading && ui.tab === "spatial");
      message.hidden = true;
      renderSpatial();
    })
    .catch((error) => {
      message.textContent = `三维视图不可用：${error.message}`;
    })
    .finally(() => {
      ui.spatialLoading = null;
    });
  return ui.spatialLoading;
}

function renderPen() {
  const state = ui.state || {};
  ui.penView?.update(
    isConnected()
      ? state
      : {
          ...state,
          connection: { ...state.connection, state: "disconnected" },
        },
  );
}

async function ensureAirPen() {
  if (ui.penView || ui.penLoading) return ui.penLoading;
  ui.penLoading = import(moduleUrl("air-pen.js"))
    .then(({ createAirPen }) => {
      ui.penView = createAirPen($("#view-pen"), {
        notify: (message, type) => toast(message, type === "error"),
        setDemoKey: (down) => {
          if (!ui.reloading && isConnected() && isDemo()) queueDemoKey(down);
        },
      });
      ui.penView.setVisible(!ui.reloading && ui.tab === "pen");
      renderPen();
      icons();
    })
    .catch((error) => toast(`空中画笔加载失败：${error.message}`, true))
    .finally(() => {
      ui.penLoading = null;
    });
  return ui.penLoading;
}

function initialTab() {
  const hashTab = window.location.hash.slice(1);
  if (TAB_NAMES.has(hashTab)) return hashTab;
  try {
    const stored = sessionStorage.getItem(TAB_STORAGE_KEY);
    if (TAB_NAMES.has(stored)) return stored;
  } catch {}
  return "library";
}

function selectTab(name) {
  if (!TAB_NAMES.has(name)) return;
  ui.tab = name;
  for (const tab of $$(".tab")) {
    const selected = tab.dataset.tab === name;
    tab.classList.toggle("active", selected);
    tab.setAttribute("aria-selected", String(selected));
  }
  for (const view of $$(".tab-view")) {
    const active = view.id === `view-${name}`;
    view.hidden = !active;
    view.classList.toggle("active", active);
  }
  try {
    sessionStorage.setItem(TAB_STORAGE_KEY, name);
  } catch {}
  if (window.location.hash !== `#${name}`) {
    history.replaceState(null, "", `#${name}`);
  }
  ui.spatialView?.setVisible(!ui.reloading && name === "spatial");
  ui.penView?.setVisible(!ui.reloading && name === "pen");
  if (name === "spatial") ensureSpatial();
  if (name === "pen") ensureAirPen();
  if (name === "live") {
    ui.lastWave = null;
    requestAnimationFrame(renderCharts);
  }
  if (name === "capture" && ui.token) refreshCaptures();
}

function renderEvents() {
  const kindNames = {
    event: "动作识别",
    recognized: "动作识别",
    recognition: "动作识别",
    unknown: "陌生动作",
    error: "错误",
    info: "设备状态",
    status: "设备状态",
    state: "设备状态",
    command: "发送命令",
    learn: "学习进度",
    train: "学习进度",
    training: "学习进度",
    calibration: "静止校准",
    saved: "保存完成",
    delete: "删除确认",
    deleted: "删除完成",
    connected: "设备连接",
    disconnected: "设备断开",
    connection: "设备连接",
    capture: "采集状态",
    hotkey: "快捷键",
    hotkeys: "快捷键",
    rejected: "拒绝动作",
    protocol: "设备回执",
    hello: "设备就绪",
    stream: "数据流",
  };
  $("#event-count").textContent = String(ui.events.length);
  $("#event-list").innerHTML = ui.events.length
    ? [...ui.events]
        .reverse()
        .map((event) => {
          const kind = String(event.kind || "info").toLowerCase();
          let time = event.time || event.timestamp || "";
          if (typeof time === "number")
            time = new Date(
              time < 1e12 ? time * 1000 : time,
            ).toLocaleTimeString("zh-CN", { hour12: false });
          else if (String(time).includes("T"))
            time = new Date(time).toLocaleTimeString("zh-CN", {
              hour12: false,
            });
          const rawMessage = event.message || event.line || event.raw || "";
          const message =
            kind === "train" && /REJECT/i.test(rawMessage)
              ? trainingMessage(rawMessage)
              : kind === "train" &&
                  /^WARN,/i.test(rawMessage) &&
                  event.training?.warning
                ? similarityMessage(event.training.warning)
                : kind === "error"
                  ? errorMessage(rawMessage)
                  : rawMessage;
          return `<div class="event-row"><span class="event-time">${escapeText(time)}</span><span class="event-kind ${["event", "recognized", "recognition"].includes(kind) ? "recognized" : escapeText(kind)}">${escapeText(kindNames[kind] || kind)}</span><span class="event-message">${escapeText(message)}</span></div>`;
        })
        .join("")
    : '<div class="event-empty">暂无事件</div>';
}

async function learnSlot(slot) {
  ui.selected = slot;
  if (!supportsManualTraining()) {
    toast("需更新 KEY 录制固件", true);
    return;
  }
  await perform(async () => {
    await command(`learn ${slot}`);
    toast(`动作 ${pad(slot)} 已准备，等待按下 KEY`);
  });
}

async function calibrateBoard() {
  await perform(async () => {
    toast("正在校准陀螺仪，请保持板子静止");
    await command("calibrate");
  });
}

function queueDemoKey(pressed) {
  const epoch = ui.state?.connection?.epoch;
  ui.demoKeyQueue = ui.demoKeyQueue
    .then(async () => {
      if (!isConnected() || !isDemo() || ui.state?.connection?.epoch !== epoch)
        return;
      await command(pressed ? "demo key 1" : "demo key 0");
    })
    .catch((error) => {
      ui.demoKeyPressed = false;
      renderState();
      toast(error.message, true);
    });
}

function pressDemoKey() {
  if (
    ui.demoKeyPressed ||
    $("#demo-key-button").disabled ||
    !isConnected() ||
    !isDemo()
  )
    return;
  ui.demoKeyPressed = true;
  renderState();
  queueDemoKey(true);
}

function releaseDemoKey() {
  if (!ui.demoKeyPressed) return;
  ui.demoKeyPressed = false;
  renderState();
  queueDemoKey(false);
}

async function requestDelete(slot) {
  ui.selected = slot;
  ui.deleteSlot = slot;
  $("#delete-description").textContent =
    `即将删除板端的「${slotById(slot)?.name || `动作 ${pad(slot)}`}」及其全部示范。`;
  $("#delete-dialog").showModal();
  await perform(async () => {
    try {
      await command(`delete ${slot}`);
    } catch (error) {
      $("#delete-dialog").close("failed");
      throw error;
    }
  });
}

function chartOptions(title) {
  return {
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    interaction: { mode: "index", intersect: false },
    plugins: {
      legend: {
        position: "top",
        align: "end",
        labels: {
          boxWidth: 12,
          boxHeight: 2,
          padding: 16,
          color: "#7b9183",
          font: { size: 10 },
        },
      },
      tooltip: {
        enabled: true,
        callbacks: {
          title: (items) => `${Number(items[0]?.parsed.x || 0).toFixed(2)} s`,
        },
      },
    },
    scales: {
      x: {
        type: "linear",
        grid: { display: false },
        border: { display: false },
        ticks: {
          maxTicksLimit: 7,
          color: "#9aaca0",
          font: { size: 9 },
          callback: (value) => `${Number(value).toFixed(1)} s`,
        },
      },
      y: {
        title: { display: false, text: title },
        grid: { color: "#e9efeb" },
        border: { display: false },
        ticks: { maxTicksLimit: 5, color: "#9aaca0", font: { size: 9 } },
      },
    },
  };
}

function createCharts() {
  if (ui.charts || !window.Chart) return;
  const colors = ["#139b88", "#d57758", "#568bce"];
  const data = () => ({
    datasets: ["X", "Y", "Z"].map((label, index) => ({
      label,
      data: [],
      borderColor: colors[index],
      backgroundColor: colors[index],
      pointRadius: 0,
      pointHitRadius: 7,
      borderWidth: 1.6,
      tension: 0.12,
    })),
  });
  ui.charts = [
    new Chart($("#accel-chart"), {
      type: "line",
      data: data(),
      options: chartOptions("m/s²"),
    }),
    new Chart($("#gyro-chart"), {
      type: "line",
      data: data(),
      options: chartOptions("rad/s"),
    }),
  ];
}

function renderCharts() {
  createCharts();
  if (!ui.charts) return;
  const waveform = isConnected() ? ui.state?.waveform || [] : [];
  const waveKey = `${ui.state?.connection?.epoch}:${waveform.length}:${waveform.at(-1)?.seq}:${waveform.at(-1)?.t}`;
  if (waveKey === ui.lastWave) return;
  ui.lastWave = waveKey;
  const axes = [
    ["ax", "ay", "az"],
    ["gx", "gy", "gz"],
  ];
  ui.charts.forEach((chart, group) => {
    chart.data.datasets.forEach((dataset, axis) => {
      dataset.data = waveform.map((sample) => ({
        x: Number(sample.t ?? sample.t_ms ?? 0) / 1000,
        y: Number(sample[axes[group][axis]]),
      }));
    });
    chart.update("none");
  });
  $("#accel-empty").hidden = waveform.length > 0;
  $("#gyro-empty").hidden = waveform.length > 0;
}

async function refreshCaptures() {
  try {
    const response = await api("/api/captures");
    const captures = response.captures || [];
    $("#capture-list").innerHTML = captures.length
      ? captures
          .map((capture) => {
            const meta = capture.metadata || {};
            const date = capture.started_utc
              ? new Date(capture.started_utc).toLocaleString("zh-CN", {
                  hour12: false,
                })
              : "";
            const details = [
              date,
              meta.user,
              meta.label,
              `${numberText(capture.raw_count || 0)} 个样本`,
              capture.simulation ? "演示数据" : "",
            ]
              .filter(Boolean)
              .map(escapeText)
              .join(" · ");
            const files = capture.files || {};
            const links = Object.entries(files)
              .map(
                ([type, name]) =>
                  `<a class="capture-download" href="/api/download?file=${encodeURIComponent(name)}" download title="下载 ${escapeText(name)}"><i data-lucide="download"></i>${escapeText({ csv: "CSV", events: "事件", summary: "摘要" }[type] || type)}</a>`,
              )
              .join("");
            return `<div class="capture-item"><span class="capture-file-icon"><i data-lucide="file-chart-column"></i></span><div class="capture-file-details"><div class="capture-file-title">${escapeText(capture.id || capture.filename || "采集记录")}</div><div class="capture-file-meta">${details}</div></div><div class="capture-downloads">${links}</div></div>`;
          })
          .join("")
      : '<div class="empty-state"><i data-lucide="folder-open"></i><span>暂无采集记录</span></div>';
    icons();
  } catch (error) {
    if (ui.tab === "capture") toast(error.message, true);
  }
}

function bindEvents() {
  $("#pose-origin-button").addEventListener("click", () =>
    perform(async () => {
      await api("/api/pose/reset", {});
      toast("空间原点已重置");
    }),
  );
  $("#pose-camera-button").addEventListener("click", () =>
    ui.spatialView?.resetCamera(),
  );
  $("#pose-trail-button").addEventListener("click", () => {
    ui.trailVisible = !ui.trailVisible;
    ui.spatialView?.setTrailVisible(ui.trailVisible);
    const button = $("#pose-trail-button");
    button.setAttribute("aria-pressed", String(ui.trailVisible));
    button.classList.toggle("active", ui.trailVisible);
    button.title = ui.trailVisible ? "隐藏运动轨迹" : "显示运动轨迹";
  });
  $("#refresh-ports").addEventListener("click", refreshPorts);
  $("#port-select").addEventListener("change", renderState);
  $("#connect-button").addEventListener("click", () =>
    perform(async () => {
      if (isConnected() || isConnecting()) await api("/api/disconnect", {});
      else
        await api("/api/connect", {
          port: $("#port-select").value,
          demo: false,
        });
    }),
  );
  $("#demo-button").addEventListener("click", () =>
    perform(async () => {
      if (isConnected()) await api("/api/disconnect", {});
      else await api("/api/connect", { demo: true, port: "" });
    }),
  );
  $("#armed-toggle").addEventListener("change", (event) => {
    const enabled = event.target.checked;
    perform(() => command(enabled ? "arm" : "disarm"));
  });
  $("#hotkeys-toggle").addEventListener("change", (event) => {
    const enabled = event.target.checked;
    perform(async () => {
      await api("/api/settings", { hotkeys_enabled: enabled });
      if (enabled) toast("快捷键已启用，作用于当前前台应用");
    });
  });
  $("#calibrate-button").addEventListener("click", calibrateBoard);
  $("#status-button").addEventListener("click", () =>
    perform(() => command("status")),
  );
  $("#stop-button").addEventListener("click", async () => {
    try {
      await command("cancel");
      toast("已停用电脑快捷键，正在停止板端动作");
    } catch (error) {
      toast(error.message, true);
    }
  });
  $("#learn-button").addEventListener("click", () => learnSlot(ui.selected));
  const demoKeyButton = $("#demo-key-button");
  demoKeyButton.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) return;
    event.preventDefault();
    if (event.isTrusted) demoKeyButton.setPointerCapture(event.pointerId);
    pressDemoKey();
  });
  demoKeyButton.addEventListener("lostpointercapture", releaseDemoKey);
  demoKeyButton.addEventListener("blur", releaseDemoKey);
  demoKeyButton.addEventListener("contextmenu", (event) =>
    event.preventDefault(),
  );
  demoKeyButton.addEventListener("keydown", (event) => {
    if (![" ", "Enter"].includes(event.key)) return;
    event.preventDefault();
    if (!event.repeat) pressDemoKey();
  });
  demoKeyButton.addEventListener("keyup", (event) => {
    if (![" ", "Enter"].includes(event.key)) return;
    event.preventDefault();
    releaseDemoKey();
  });
  document.addEventListener("pointerup", releaseDemoKey);
  document.addEventListener("pointercancel", releaseDemoKey);
  document.addEventListener("keyup", (event) => {
    if ([" ", "Enter"].includes(event.key)) releaseDemoKey();
  });
  window.addEventListener("blur", releaseDemoKey);
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) releaseDemoKey();
  });
  $("#save-button").addEventListener("click", () =>
    perform(() => command("save")),
  );
  $("#cancel-button").addEventListener("click", () =>
    perform(() => command("cancel")),
  );
  $("#delete-confirm").addEventListener("click", () =>
    perform(async () => {
      await command("save");
      $("#delete-dialog").close("deleted");
    }),
  );
  $("#delete-dialog").addEventListener("close", () => {
    const result = $("#delete-dialog").returnValue;
    if (result !== "deleted" && result !== "failed" && isConnected())
      command("cancel").catch((error) => toast(error.message, true));
    ui.deleteSlot = null;
  });
  $("#clear-events").addEventListener("click", () => {
    ui.events = [];
    renderEvents();
  });
  for (const button of $$(".tab"))
    button.addEventListener("click", () => selectTab(button.dataset.tab));
  window.addEventListener("hashchange", () => selectTab(initialTab()));
  $("#refresh-captures").addEventListener("click", refreshCaptures);
  $("#capture-form").addEventListener("submit", (event) => {
    event.preventDefault();
    perform(async () => {
      if (ui.state?.capture?.active) {
        await api("/api/capture/stop", {});
        toast("采集记录已保存");
      } else
        await api(
          "/api/capture/start",
          Object.fromEntries(new FormData(event.currentTarget)),
        );
      await refreshCaptures();
    });
  });
  $(".board-photo").addEventListener("error", (event) => {
    event.target.style.visibility = "hidden";
  });
}

async function poll() {
  if (ui.polling || !ui.token || ui.reloading) return;
  const now = performance.now();
  if (now - ui.lastPollAt < (ui.tab === "pen" ? 45 : 190)) return;
  ui.lastPollAt = now;
  ui.polling = true;
  try {
    applyState(await api(`/api/state?after=${ui.cursor}`));
  } catch (error) {
    ui.serverError = `本地服务连接失败：${error.message}`;
    renderState();
  } finally {
    ui.polling = false;
  }
}

async function initialize() {
  createSlotRows();
  bindEvents();
  selectTab(initialTab());
  const now = new Date();
  $("#capture-session").value =
    `s${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}`;
  renderState();
  try {
    const session = await api("/api/session.json");
    ui.token = session.token;
    await Promise.all([refreshPorts(), refreshCaptures(), poll()]);
    setInterval(poll, 50);
  } catch (error) {
    ui.serverError = `无法连接本地服务：${error.message}`;
    renderState();
  }
}

initialize();
