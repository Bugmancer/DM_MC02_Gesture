const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { chromium } = require("playwright");
const { PNG } = require("pngjs");

const root = path.resolve(__dirname, "..");
const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), "gesture-gui-test-"));
const screenshots = path.join(__dirname, "screenshots");
fs.mkdirSync(screenshots, { recursive: true });
const python =
  process.env.GESTURE_PYTHON ||
  path.join(root, ".venv", "Scripts", "python.exe");
const server = spawn(
  python,
  [path.join(root, "server.py"), "--port", "0", "--data-dir", dataDir],
  { windowsHide: true },
);
let serverErrors = "";
server.stderr.on("data", (data) => {
  serverErrors += data;
});
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
let browser, url, token;

async function api(route, payload) {
  assert.notEqual(
    new URL(url).port,
    "8765",
    "Never use the live device server",
  );
  const response = await fetch(
    url + route,
    payload === undefined
      ? {}
      : {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            "X-Gesture-Token": token,
          },
          body: JSON.stringify(payload),
        },
  );
  assert.equal(response.status, 200, await response.clone().text());
  return response.json();
}

async function noPageOverflow(page) {
  const overflow = await page.evaluate(() => ({
    width: innerWidth,
    scroll: document.documentElement.scrollWidth,
    containers: [...document.querySelectorAll("html, body, .workspace, .table-scroll, .tabs, dialog, #toast")]
      .map(element => ({tag: element.tagName, id: element.id, className: element.className,
        width: element.getBoundingClientRect().width, right: element.getBoundingClientRect().right,
        scroll: element.scrollWidth, overflow: getComputedStyle(element).overflow})),
    elements: [...document.querySelectorAll("body *")]
      .filter(element => (element.getBoundingClientRect().right > innerWidth + 1 ||
        (element.scrollWidth > element.clientWidth + 1 && getComputedStyle(element).overflow === "visible")) &&
        !element.closest(".table-scroll"))
      .slice(0, 12).map(element => ({tag: element.tagName, id: element.id,
        className: String(element.className), right: element.getBoundingClientRect().right,
        client: element.clientWidth, scroll: element.scrollWidth})),
  }));
  assert.equal(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
    true,
    `Page exceeds viewport width: ${JSON.stringify(overflow)}`,
  );
}

function scenePixels(buffer) {
  const png = PNG.sync.read(buffer);
  let colored = 0;
  let dark = 0;
  const axes = [0, 0, 0];
  for (let i = 0; i < png.data.length; i += 4) {
    const [r, g, b] = png.data.subarray(i, i + 3);
    if (Math.max(r, g, b) - Math.min(r, g, b) > 35) colored++;
    if (Math.max(r, g, b) < 150) dark++;
    if (r > g * 1.3 && r > b * 1.3) axes[0]++;
    if (g > r * 1.3 && g > b * 1.12) axes[1]++;
    if (b > r * 1.3 && b > g * 1.2) axes[2]++;
  }
  return { colored, dark, axes };
}

async function onlyView(page, name) {
  assert.deepEqual(
    await page
      .locator(".tab-view:visible")
      .evaluateAll((views) => views.map((view) => view.id)),
    [`view-${name}`],
    "Exactly the selected view must remain visible",
  );
  assert.equal(new URL(page.url()).hash, `#${name}`);
}

async function verifyViewRecovery() {
  const page = await browser.newPage({
    viewport: { width: 1440, height: 1000 },
  });
  const baselineSession = await api("/api/session.json");
  const mock = await api("/api/state");
  const originalRevision = baselineSession.ui_revision;
  let revision = originalRevision;
  let instance = baselineSession.server_instance;
  let documentLoads = 0;
  const mutations = [];
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  mock.connection = {
    ...mock.connection,
    state: "disconnected",
    demo: false,
    simulation: false,
  };
  const draftKey = "dm-mc02-air-pen-world-v1";
  const draft = {
    version: 4,
    dimensions: 3,
    units: "m",
    reference_frame: {
      axes: "gravity-aligned",
      up_axis: "z",
      heading: "relative-yaw",
      origin: "local-inertial",
    },
    strokes: [
      {
        color: "#397fc5",
        width: 3,
        frame: "test",
        points: [
          [0, 0, 0],
          [0.08, 0.05, 0.03],
        ],
      },
    ],
    color: "#397fc5",
    width: 3,
    sensitivity: 1,
  };
  await page.addInitScript(
    ({ key, draft }) => {
      if (!localStorage.getItem(key))
        localStorage.setItem(key, JSON.stringify(draft));
    },
    { key: draftKey, draft },
  );
  await page.route(
    (requestUrl) => requestUrl.pathname === "/",
    async (route) => {
      documentLoads++;
      const response = await route.fetch();
      await route.fulfill({
        response,
        body: (await response.text()).replaceAll(originalRevision, revision),
      });
    },
  );
  await page.route("**/api/session.json", (route) =>
    route.fulfill({
      json: {
        ...baselineSession,
        ui_revision: revision,
        server_instance: instance,
      },
    }),
  );
  await page.route("**/api/state*", (route) =>
    route.fulfill({
      json: {
        ...mock,
        ui_revision: revision,
        server_instance: instance,
      },
    }),
  );
  await page.route("**/api/disconnect", (route) => {
    mutations.push("disconnect");
    mock.connection.state = "disconnected";
    return route.fulfill({
      json: {
        ok: true,
        state: { ...mock, ui_revision: revision, server_instance: instance },
      },
    });
  });
  await page.route("**/api/**", (route) => {
    if (
      route.request().method() === "POST" &&
      new URL(route.request().url()).pathname !== "/api/disconnect"
    ) {
      mutations.push(new URL(route.request().url()).pathname);
      return route.fulfill({
        status: 400,
        json: { ok: false, error: "Unexpected mutation in view test" },
      });
    }
    return route.fallback();
  });
  async function restoredPen() {
    await page.waitForFunction(() =>
      document
        .querySelector("#pen-stroke-count")
        ?.textContent.startsWith("1 笔"),
    );
    await onlyView(page, "pen");
    assert.deepEqual(
      await page.evaluate(
        (key) => JSON.parse(localStorage.getItem(key)).strokes,
        draftKey,
      ),
      draft.strokes,
    );
    assert.notEqual(await page.locator("#pen-status").textContent(), "记录中");
  }
  await page.goto(url + "/#pen");
  await restoredPen();
  await page.reload();
  await restoredPen();

  let releaseModule;
  let moduleRequested;
  const requested = new Promise((resolve) => {
    moduleRequested = resolve;
  });
  const heldModule = new Promise((resolve) => {
    releaseModule = resolve;
  });
  await page.route("**/spatial.js*", async (route) => {
    moduleRequested();
    await heldModule;
    await route.continue();
  });
  await page.locator('[data-tab="spatial"]').click();
  await requested;
  await page.locator('[data-tab="pen"]').click();
  releaseModule();
  await page.waitForFunction(
    () => document.querySelectorAll("#spatial-scene canvas").length === 1,
  );
  await onlyView(page, "pen");
  await page.unroute("**/spatial.js*");

  let count = documentLoads;
  revision = "test-ui-revision-2";
  await page.waitForFunction(
    (value) =>
      document.querySelector("script[data-ui-revision]")?.dataset.uiRevision ===
      value,
    revision,
  );
  await restoredPen();
  await delay(700);
  assert.equal(
    documentLoads,
    count + 1,
    "Asset update must reload exactly once",
  );
  count = documentLoads;
  const refreshedSession = page.waitForResponse(
    async (response) =>
      new URL(response.url()).pathname === "/api/session.json" &&
      (await response.json()).server_instance === "test-restarted-service",
  );
  instance = "test-restarted-service";
  await refreshedSession;
  await restoredPen();
  await delay(700);
  assert.equal(
    documentLoads,
    count + 1,
    "Service restart must reload exactly once",
  );

  for (const state of ["connecting", "reconnecting"]) {
    mock.connection = {
      ...mock.connection,
      state,
      port: "COM_TEST",
      retry_attempt: 2,
      retry_limit: 5,
      error: "",
    };
    const label = state === "reconnecting" ? "取消重连" : "取消连接";
    await page.waitForFunction(
      (label) =>
        document.querySelector("#connect-button span").textContent === label,
      label,
    );
    assert.equal(await page.locator("#connect-button").isEnabled(), true);
    assert.equal(await page.locator("#port-select").isDisabled(), true);
    assert.equal(await page.locator("#demo-button").isDisabled(), true);
    await page.locator("#connect-button").click();
    await page.waitForFunction(
      () =>
        document.querySelector("#connect-button span").textContent ===
        "连接设备",
    );
  }
  assert.deepEqual(mutations, ["disconnect", "disconnect"]);
  assert.deepEqual(errors, [], "Recovery page errors");
  await page.close();
  return { reloads: documentLoads, preservedStrokes: draft.strokes.length };
}

function changedPixels(first, second) {
  const a = PNG.sync.read(first);
  const b = PNG.sync.read(second);
  assert.equal(a.data.length, b.data.length);
  let count = 0;
  for (let i = 0; i < a.data.length; i += 4) {
    if (
      Math.abs(a.data[i] - b.data[i]) +
        Math.abs(a.data[i + 1] - b.data[i + 1]) +
        Math.abs(a.data[i + 2] - b.data[i + 2]) >
      60
    )
      count++;
  }
  return count;
}

(async () => {
  for (let attempt = 0; attempt < 100; attempt++) {
    const metadata = path.join(dataDir, "server.json");
    if (fs.existsSync(metadata)) {
      url = JSON.parse(fs.readFileSync(metadata, "utf8")).url;
      break;
    }
    if (server.exitCode !== null) throw new Error(serverErrors);
    await delay(100);
  }
  assert.ok(url, "Server did not become ready");
  token = (await api("/api/session.json")).token;
  browser = await chromium.launch({ channel: "chrome", headless: true });
  const page = await browser.newPage({
    viewport: { width: 1440, height: 1000 },
    deviceScaleFactor: 1,
  });
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("console", (message) => {
    if (message.type() === "error") errors.push(message.text());
  });
  await page.goto(url);
  await page.waitForFunction(
    () => document.querySelectorAll("#slot-rows tr").length === 8,
  );
  await page.screenshot({
    path: path.join(screenshots, "desktop-disconnected.png"),
    fullPage: true,
  });
  await page.locator("#demo-button").click();
  await page.waitForFunction(
    () =>
      document.querySelector('#slot-rows tr[data-slot="3"] .learn-slot')
        .disabled === false,
  );
  assert.ok(
    (await page.locator("#firmware-version").textContent()).includes(
      "gesture-20261006-r8",
    ),
  );
  assert.equal(await page.locator("#firmware-warning").isHidden(), true);
  assert.equal(await page.locator("#hotkeys-toggle").isDisabled(), true);
  assert.equal(await page.locator("#armed-toggle").isDisabled(), true);
  assert.equal(await page.locator("#armed-toggle").isChecked(), true);
  await page.locator("#config-class-limit").selectOption("5");
  await page.locator("#config-demo-target").selectOption("2");
  await page.locator('[data-slot="1"] .slot-color-input').fill("#123456");
  await delay(700);
  assert.equal(await page.locator("#config-class-limit").inputValue(), "5");
  assert.equal(await page.locator("#config-demo-target").inputValue(), "2");
  assert.equal((await api("/api/state")).config.class_limit, 8,
    "An unsaved draft must not change the board");
  assert.equal(await page.locator("#config-save").isEnabled(), true);
  await page.locator("#config-save").click();
  await page.waitForFunction(() =>
    document.querySelector("#config-save-status").textContent.includes("已保存"));
  let configured = await api("/api/state");
  assert.equal(configured.config.class_limit, 5);
  assert.equal(configured.config.demo_target, 2);
  assert.equal(configured.slots[0].color, "#123456");
  assert.equal(configured.slots[0].state, "saved",
    "Changing settings must preserve existing templates");
  assert.equal(configured.status.armed, 1);
  assert.equal(await page.locator("#slot-rows tr:visible").count(), 5);
  assert.equal(await page.locator("#config-save").isDisabled(), true);
  await page.screenshot({path: path.join(screenshots, "desktop-configured.png"), fullPage: true});
  await page.setViewportSize({width: 390, height: 844});
  await page.screenshot({path: path.join(screenshots, "mobile-configured.png"), fullPage: true});
  await noPageOverflow(page);
  await page.setViewportSize({width: 1440, height: 1000});
  await page.locator("#config-class-limit").selectOption("8");
  await page.locator("#config-demo-target").selectOption("3");
  await page.locator("#config-save").click();
  await page.waitForFunction(() =>
    document.querySelector("#slot-rows tr[data-slot='8']").hidden === false &&
    document.querySelector("#config-save").disabled);
  configured = await api("/api/state");
  assert.equal(configured.config.class_limit, 8);
  assert.equal(configured.config.demo_target, 3);
  assert.equal(configured.slots[0].color, "#123456");
  const third = page.locator('#slot-rows tr[data-slot="3"]');
  await third.locator(".name-input").click();
  await third.locator(".name-input").fill("Circle test");
  await third.locator(".name-input").press("Enter");
  await page.waitForFunction(
    () =>
      document.querySelector("#training-title").textContent === "Circle test",
  );
  await third.locator(".hotkey-input").fill("ctrl+shift+k");
  await Promise.all([
    page.waitForResponse(
      (response) =>
        response.url().endsWith("/api/settings") &&
        response.request().method() === "POST",
    ),
    third.locator(".hotkey-input").press("Enter"),
  ]);
  await page.waitForFunction(
    () =>
      document.querySelector('#slot-rows tr[data-slot="3"] .hotkey-input')
        .value === "ctrl+shift+k",
  );
  let state = await api("/api/state");
  assert.equal(state.slots[2].name, "Circle test");
  assert.equal(state.slots[2].hotkey, "ctrl+shift+k");
  await third.locator(".learn-slot").click();
  await page.waitForFunction(
    () =>
      !document.querySelector("#demo-key-button").hidden &&
      !document.querySelector("#demo-key-button").disabled,
  );
  await delay(450);
  assert.equal(
    (await api("/api/state")).training.collected,
    0,
    "Waiting must not record demonstrations",
  );
  for (let count = 1; count <= 3; count++) {
    const key = await page.locator("#demo-key-button").boundingBox();
    await page.mouse.move(key.x + key.width / 2, key.y + key.height / 2);
    await page.mouse.down();
    await page.waitForFunction(() =>
      document
        .querySelector("#training-status")
        .textContent.includes("正在录制"),
    );
    await delay(350);
    assert.equal(
      (await api("/api/state")).training.collected,
      count - 1,
      "Held KEY must not finish recording",
    );
    assert.equal(
      await page.locator("#save-button").isDisabled(),
      true,
      "Saving must not interrupt an active KEY capture",
    );
    await page.mouse.up();
    await page.waitForFunction(
      (value) =>
        Number(document.querySelector("#training-count").textContent) === value,
      count,
    );
    if (count < 3) {
      assert.equal(await page.locator("#save-button").isDisabled(), true);
      assert.equal(
        await page.locator("#demo-key-button").isDisabled(),
        false,
        "The configured target still needs another demonstration",
      );
    }
  }
  await page.waitForFunction(() =>
    document
      .querySelector('#slot-rows tr[data-slot="3"] .slot-state-text')
      .textContent.includes("已保存"),
  );
  assert.equal(
    (await api("/api/state")).slots[2].state,
    "saved",
    "The third configured demonstration must automatically save",
  );
  await page.screenshot({
    path: path.join(screenshots, "desktop-ready.png"),
    fullPage: true,
  });
  assert.equal((await api("/api/state")).status.armed, 1);
  await page.locator('#slot-rows tr[data-slot="4"] .learn-slot').click();
  await page.waitForFunction(
    () => !document.querySelector("#demo-key-button").disabled,
  );
  const singleKey = await page.locator("#demo-key-button").boundingBox();
  await page.mouse.move(
    singleKey.x + singleKey.width / 2,
    singleKey.y + singleKey.height / 2,
  );
  await page.mouse.down();
  await page.waitForFunction(() =>
    document.querySelector("#training-status").textContent.includes("正在录制"),
  );
  await delay(350);
  await page.mouse.up();
  await page.waitForFunction(
    () =>
      document.querySelector("#training-count").textContent === "1" &&
      document.querySelector("#save-button").disabled,
  );
  assert.equal(
    (await api("/api/state")).slots[3].state,
    "empty",
    "A partial three-demonstration session must not auto-save early",
  );
  await page.locator("#cancel-button").click();
  await page.waitForFunction(
    () => document.querySelector("#armed-toggle").checked,
  );
  await page.locator('#slot-rows tr[data-slot="5"] .learn-slot').click();
  await page.waitForFunction(
    () => !document.querySelector("#cancel-button").disabled,
  );
  await page.locator("#cancel-button").click();
  await page.waitForFunction(
    () => document.querySelector("#training-count").textContent === "0",
  );
  await third.locator(".delete-button").click();
  await page.waitForFunction(
    () => !document.querySelector("#delete-confirm").disabled,
  );
  await page.locator("#delete-cancel").click();
  await page.waitForFunction(
    () => !document.querySelector("#delete-dialog").open,
  );
  assert.equal((await api("/api/state")).slots[2].state, "saved");
  await third.locator(".delete-button").click();
  await page.waitForFunction(
    () => !document.querySelector("#delete-confirm").disabled,
  );
  await page.locator("#delete-confirm").click();
  await page.waitForFunction(() =>
    document
      .querySelector('#slot-rows tr[data-slot="3"] .slot-state-text')
      .textContent.includes("空槽位"),
  );
  assert.equal(await page.locator("#armed-toggle").isDisabled(), true);
  await page.waitForFunction(
    () => document.querySelector("#armed-toggle").checked,
  );
  await page.waitForFunction(
    () => !document.querySelector("#live-match-color").hidden,
  );
  assert.ok(
    (await api("/api/state")).protocol.capabilities.includes("LIVE_MATCH"),
  );
  await page.waitForFunction(
    () => document.querySelector("#metric-last").textContent !== "—",
    { timeout: 7000 },
  );
  await page.locator('[data-tab="live"]').click();
  await page.waitForFunction(
    () => document.querySelector("#accel-empty").hidden,
  );
  const pixels = await page.evaluate(() =>
    [...document.querySelectorAll("#accel-chart, #gyro-chart")].map(
      (canvas) => {
        const rgba = canvas
          .getContext("2d")
          .getImageData(0, 0, canvas.width, canvas.height).data;
        let colored = 0;
        for (let i = 0; i < rgba.length; i += 4)
          if (
            rgba[i + 3] &&
            Math.max(rgba[i], rgba[i + 1], rgba[i + 2]) -
              Math.min(rgba[i], rgba[i + 1], rgba[i + 2]) >
              40
          )
            colored++;
        return colored;
      },
    ),
  );
  assert.ok(
    pixels.every((count) => count > 300),
    `Nonblank charts expected: ${pixels}`,
  );
  await noPageOverflow(page);
  await page.screenshot({
    path: path.join(screenshots, "desktop-live.png"),
    fullPage: true,
  });
  await page.locator('[data-tab="spatial"]').click();
  const scene = page.locator("#spatial-scene canvas");
  await scene.waitFor({ state: "visible" });
  await page.waitForFunction(
    () => document.querySelector("#pose-pitch").textContent !== "—",
  );
  const spatialState = await api("/api/state");
  spatialState.pose = {
    ...spatialState.pose,
    available: true,
    position_m: [0.01, 0.02, 0.01],
    trail: [
      [0, 0, 0],
      [0.01, 0.02, 0.01],
    ],
  };
  await page.route("**/api/state*", (route) =>
    route.fulfill({ json: spatialState }),
  );
  await page.waitForFunction(
    () => document.querySelector("#pose-x").textContent === "0.010",
  );
  await delay(300);
  const sceneBefore = await scene.screenshot();
  const spatialPixels = scenePixels(sceneBefore);
  assert.ok(
    spatialPixels.colored > 200 &&
      spatialPixels.axes.every((count) => count > 20),
    `Fixed world axes and trajectory expected: ${JSON.stringify(spatialPixels)}`,
  );
  spatialState.pose.position_m = [0.06, 0.03, 0.02];
  spatialState.pose.trail.push([0.06, 0.03, 0.02]);
  await page.waitForFunction(
    () => document.querySelector("#pose-x").textContent === "0.060",
  );
  await delay(300);
  const motionPixels = changedPixels(sceneBefore, await scene.screenshot());
  assert.ok(
    motionPixels > 80,
    `Sensor-driven position marker and trail motion expected: ${motionPixels}`,
  );
  const sceneBox = await scene.boundingBox();
  await page.mouse.move(
    sceneBox.x + sceneBox.width / 2,
    sceneBox.y + sceneBox.height / 2,
  );
  await page.mouse.down();
  await page.mouse.move(
    sceneBox.x + sceneBox.width / 2 + 100,
    sceneBox.y + sceneBox.height / 2 + 40,
    { steps: 12 },
  );
  await page.mouse.up();
  await page.mouse.wheel(0, 100);
  await page.locator("#pose-camera-button").click();
  await page.locator("#pose-trail-button").click();
  assert.equal(
    await page.locator("#pose-trail-button").getAttribute("aria-pressed"),
    "false",
  );
  await page.locator("#pose-trail-button").click();
  assert.equal(
    await page.locator("#pose-trail-button").getAttribute("aria-pressed"),
    "true",
  );
  await page.unroute("**/api/state*");
  const resetResponse = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/pose/reset") &&
      response.request().method() === "POST",
  );
  await page.locator("#pose-origin-button").click();
  assert.equal((await resetResponse).status(), 200);
  assert.ok((await api("/api/state")).pose.elapsed_s < 1);
  await noPageOverflow(page);
  await page.screenshot({
    path: path.join(screenshots, "desktop-spatial.png"),
    fullPage: true,
  });
  await page.locator('[data-tab="capture"]').click();
  await page.locator("#capture-label").fill("qa-demo");
  await page.locator("#capture-button").click();
  await page.waitForFunction(
    () => document.querySelector("#capture-badge").textContent === "正在采集",
  );
  await page.waitForFunction(() =>
    /[1-9]\d* 个样本/.test(
      document.querySelector("#capture-current-text").textContent,
    ),
  );
  await page.locator("#capture-button").click();
  await page.waitForFunction(
    () => document.querySelectorAll(".capture-download").length === 3,
  );
  const downloadPromise = page.waitForEvent("download");
  await page.locator(".capture-download").first().click();
  const download = await downloadPromise;
  assert.ok(download.suggestedFilename().endsWith(".csv"));
  assert.equal(await download.failure(), null);
  await page.screenshot({
    path: path.join(screenshots, "desktop-capture.png"),
    fullPage: true,
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await noPageOverflow(page);
  await page.screenshot({
    path: path.join(screenshots, "mobile-capture.png"),
    fullPage: true,
  });
  await page.locator('[data-tab="library"]').click();
  await noPageOverflow(page);
  await page.screenshot({
    path: path.join(screenshots, "mobile-library.png"),
    fullPage: true,
  });
  await page.locator('[data-tab="live"]').click();
  await noPageOverflow(page);
  await page.screenshot({
    path: path.join(screenshots, "mobile-live.png"),
    fullPage: true,
  });
  await page.locator('[data-tab="spatial"]').click();
  await delay(400);
  await noPageOverflow(page);
  const mobileSpatialPixels = scenePixels(await scene.screenshot());
  assert.ok(
    mobileSpatialPixels.colored > 80 &&
      mobileSpatialPixels.axes.every((count) => count > 5),
    `Mobile fixed coordinate axes expected: ${JSON.stringify(mobileSpatialPixels)}`,
  );
  await page.screenshot({
    path: path.join(screenshots, "mobile-spatial.png"),
    fullPage: true,
  });
  const warningState = await api("/api/state");
  warningState.config = { class_limit: 5, demo_target: 1, reported: true };
  warningState.slots.forEach((slot) => {
    slot.enabled = slot.id <= 5;
  });
  warningState.slots[0].color = "#18000a";
  warningState.status.armed = 0;
  warningState.training = {
    state: "ready",
    slot: 5,
    collected: 1,
    required: 1,
    capturing: false,
    message: "",
    warning: { kind: "SIMILAR", slot: 1, distance: 0.12, limit: 0.17 },
  };
  await page.route("**/api/state*", (route) =>
    route.fulfill({ json: warningState }),
  );
  await page.locator('[data-tab="library"]').click();
  await page.locator("#training-warning").waitFor({ state: "visible" });
  assert.ok(
    (await page.locator("#training-warning").textContent()).includes(
      "示范已录入，可保存",
    ),
  );
  assert.ok(
    (await page.locator("#training-warning").textContent()).includes("动作 01"),
  );
  assert.equal(await page.locator("#training-count").textContent(), "1");
  assert.equal(await page.locator("#save-button").isDisabled(), false);
  assert.equal(await page.locator("#slot-rows tr:visible").count(), 5);
  assert.equal(await page.locator(".sample-step:visible").count(), 1);
  assert.equal(await page.locator("#slot-limit-badge").textContent(), "5");
  assert.equal(
    await page
      .locator('#slot-rows tr[data-slot="1"] .color-swatch')
      .evaluate((element) => getComputedStyle(element).backgroundColor),
    "rgb(24, 0, 10)",
  );
  await noPageOverflow(page);
  await page.screenshot({
    path: path.join(screenshots, "mobile-similarity-warning.png"),
    fullPage: true,
  });
  await page.setViewportSize({ width: 1440, height: 1000 });
  await noPageOverflow(page);
  await page.screenshot({
    path: path.join(screenshots, "desktop-similarity-warning.png"),
    fullPage: true,
  });
  await page.unroute("**/api/state*");
  await page.locator("#connect-button").click();
  await page.waitForFunction(
    () => document.querySelector("#metric-armed").textContent === "等待连接",
  );
  assert.deepEqual(errors, [], "Browser console errors");
  const viewRecovery = await verifyViewRecovery();
  console.log(
    JSON.stringify(
      {
        result: "passed",
        chartsColoredPixels: pixels,
        spatialPixels,
        motionPixels,
        mobileSpatialPixels,
        viewRecovery,
        screenshots,
        dataDir,
      },
      null,
      2,
    ),
  );
})()
  .catch((error) => {
    console.error(error);
    process.exitCode = 1;
  })
  .finally(async () => {
    if (browser) await browser.close();
    if (url && token) await api("/api/shutdown", {}).catch(() => {});
    if (server.exitCode === null) {
      await Promise.race([
        new Promise((resolve) => server.once("exit", resolve)),
        delay(3000),
      ]);
      if (server.exitCode === null) server.kill();
    }
  });
