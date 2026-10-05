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
  assert.equal(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
    true,
    "Page exceeds viewport width",
  );
}

function scenePixels(buffer) {
  const png = PNG.sync.read(buffer);
  let colored = 0;
  let dark = 0;
  for (let i = 0; i < png.data.length; i += 4) {
    const [r, g, b] = png.data.subarray(i, i + 3);
    if (Math.max(r, g, b) - Math.min(r, g, b) > 35) colored++;
    if (Math.max(r, g, b) < 150) dark++;
  }
  return { colored, dark };
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
      "gesture-20261005-r5",
    ),
  );
  assert.equal(await page.locator("#firmware-warning").isHidden(), true);
  assert.equal(await page.locator("#hotkeys-toggle").isDisabled(), true);
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
    await page.waitForFunction(
      () => !document.querySelector("#save-button").disabled,
    );
    if (count < 3) {
      assert.equal(
        await page.locator("#demo-key-button").isDisabled(),
        false,
        "Extra demonstrations must remain optional after the first",
      );
    }
  }
  await page.waitForFunction(
    () => !document.querySelector("#save-button").disabled,
  );
  assert.equal(
    (await api("/api/state")).slots[2].state,
    "empty",
    "Training requires explicit save",
  );
  await page.screenshot({
    path: path.join(screenshots, "desktop-ready.png"),
    fullPage: true,
  });
  await page.locator("#save-button").click();
  await page.waitForFunction(() =>
    document
      .querySelector('#slot-rows tr[data-slot="3"] .slot-state-text')
      .textContent.includes("已保存"),
  );
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
      !document.querySelector("#save-button").disabled,
  );
  await page.locator("#save-button").click();
  await page.waitForFunction(() =>
    document
      .querySelector('#slot-rows tr[data-slot="4"] .slot-state-text')
      .textContent.includes("已保存"),
  );
  assert.equal(
    (await api("/api/state")).slots[3].templates,
    1,
    "One real demonstration must save as one template, not three copies",
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
  await page.locator("#armed-toggle").click();
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
  await delay(400);
  const sceneBefore = await scene.screenshot();
  const spatialPixels = scenePixels(sceneBefore);
  assert.ok(
    spatialPixels.colored > 200 && spatialPixels.dark > 1000,
    `Rendered board and axes expected: ${JSON.stringify(spatialPixels)}`,
  );
  await delay(800);
  const motionPixels = changedPixels(sceneBefore, await scene.screenshot());
  assert.ok(
    motionPixels > 500,
    `Sensor-driven scene motion expected: ${motionPixels}`,
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
    mobileSpatialPixels.colored > 100 && mobileSpatialPixels.dark > 300,
    `Mobile board expected: ${JSON.stringify(mobileSpatialPixels)}`,
  );
  await page.screenshot({
    path: path.join(screenshots, "mobile-spatial.png"),
    fullPage: true,
  });
  const warningState = await api("/api/state");
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
  console.log(
    JSON.stringify(
      {
        result: "passed",
        chartsColoredPixels: pixels,
        spatialPixels,
        motionPixels,
        mobileSpatialPixels,
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
