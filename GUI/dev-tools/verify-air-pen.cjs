const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { chromium } = require("playwright");
const { PNG } = require("pngjs");

const root = path.resolve(__dirname, "..");
const dataDir = fs.mkdtempSync(
  path.join(os.tmpdir(), "gesture-air-pen-3d-test-"),
);
const screenshots = path.join(__dirname, "screenshots");
fs.mkdirSync(screenshots, { recursive: true });
const server = spawn(
  process.env.GESTURE_PYTHON ||
    path.join(root, ".venv", "Scripts", "python.exe"),
  [path.join(root, "server.py"), "--port", "0", "--data-dir", dataDir],
  { windowsHide: true },
);
let serverErrors = "";
server.stderr.on("data", (data) => {
  serverErrors += data;
});
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const draftKey = "dm-mc02-air-pen-world-v1";
let browser, url, token, diagnosticPage;

async function api(route, payload) {
  assert.ok(url && !url.match(/:8765\/?$/), "Never use the user's live server");
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

async function draft(page) {
  return page.evaluate(
    (key) => JSON.parse(localStorage.getItem(key)) || { strokes: [] },
    draftKey,
  );
}

async function waitDraft(page, predicate, message) {
  for (let attempt = 0; attempt < 70; attempt++) {
    const value = await draft(page);
    if (predicate(value)) return value;
    await delay(100);
  }
  assert.fail(message || "Draft did not reach the expected state");
}

async function recording(page, expected) {
  await page.waitForFunction(
    (value) =>
      (document.querySelector("#pen-status").textContent === "记录中") ===
      value,
    expected,
  );
}

async function setRange(page, selector, value) {
  await page.locator(selector).evaluate((input, number) => {
    input.value = String(number);
    input.dispatchEvent(new Event("input", { bubbles: true }));
  }, value);
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

function countPixels(buffer) {
  const png = PNG.sync.read(buffer);
  let ink = 0,
    blue = 0;
  for (let index = 0; index < png.data.length; index += 4) {
    const [r, g, b] = png.data.subarray(index, index + 3);
    if (Math.min(r, g, b) < 200) ink++;
    if (b > r + 45 && b > g + 20) blue++;
  }
  return { ink, blue };
}

function changedPixels(first, second) {
  const a = PNG.sync.read(first),
    b = PNG.sync.read(second);
  assert.equal(a.data.length, b.data.length);
  let count = 0;
  for (let index = 0; index < a.data.length; index += 4) {
    if (
      Math.abs(a.data[index] - b.data[index]) +
        Math.abs(a.data[index + 1] - b.data[index + 1]) +
        Math.abs(a.data[index + 2] - b.data[index + 2]) >
      60
    )
      count++;
  }
  return count;
}

async function dragCanvas(page, canvas) {
  const box = await canvas.boundingBox();
  await page.mouse.move(box.x + box.width * 0.4, box.y + box.height * 0.5);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width * 0.7, box.y + box.height * 0.35, {
    steps: 12,
  });
  await page.mouse.up();
  await delay(350);
}

async function downloadFile(page, selector) {
  const pending = page.waitForEvent("download");
  await page.locator(selector).click();
  const download = await pending;
  assert.equal(await download.failure(), null);
  return {
    name: download.suggestedFilename(),
    data: fs.readFileSync(await download.path()),
  };
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
  assert.ok(url, "Isolated server did not become ready");
  token = (await api("/api/session.json")).token;
  browser = await chromium.launch({ channel: "chrome", headless: true });
  const page = await browser.newPage({
    viewport: { width: 1440, height: 1100 },
    deviceScaleFactor: 1,
  });
  diagnosticPage = page;
  page.setDefaultTimeout(10000);
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("console", (message) => {
    if (message.type() === "error") errors.push(message.text());
  });
  await page.goto(url);
  await page.locator('[data-tab="pen"]').click();
  const canvas = page.locator("#air-pen-canvas");
  await canvas.waitFor({ state: "visible" });
  assert.equal(
    await page.locator("#pen-toggle").count(),
    0,
    "A browser start button must not bypass physical KEY",
  );
  assert.equal(await page.locator("#pen-demo-key").isHidden(), true);
  await page.locator("#demo-button").click();
  await page.waitForFunction(
    () =>
      !document.querySelector("#pen-demo-key").disabled &&
      !document.querySelector("#pen-demo-key").hidden,
  );
  await delay(700);
  await dragCanvas(page, canvas);
  assert.deepEqual(
    (await draft(page)).strokes,
    [],
    "Motion and mouse orbit without KEY must not create ink",
  );
  assert.equal(await page.locator("#pen-export").isDisabled(), true);

  const keyBox = await page.locator("#pen-demo-key").boundingBox();
  await page.mouse.move(
    keyBox.x + keyBox.width / 2,
    keyBox.y + keyBox.height / 2,
  );
  await page.mouse.down();
  await recording(page, true);
  await delay(1100);
  await page.mouse.up();
  await recording(page, false);
  const liveDraft = await waitDraft(
    page,
    (value) =>
      value.strokes.length === 1 && value.strokes[0].points.length > 10,
    "Demo RAW samples through the pose estimator must draw while KEY is held",
  );
  await delay(600);
  assert.deepEqual(
    (await draft(page)).strokes,
    liveDraft.strokes,
    "Releasing KEY must stop recording",
  );

  // Deliver known 3D IMU estimates through the same state API as a connected board.
  const mock = await api("/api/state");
  mock.pen_epoch += 100;
  mock.pen_cursor = 0;
  mock.pen_samples = [];
  mock.pose.available = true;
  mock.pose.status = "tracking";
  mock.pose.position_limited = false;
  mock.connection.demo = false;
  mock.key = { available: true, down: false };
  mock.pen_motion = { available: true, blocked: false, reason: "ready" };
  let sequence = 0,
    responses = 0,
    keyDown = false,
    queuedKeys = null,
    fixedPosition = null;
  const worldPoints = new Set();
  await page.route("**/api/state*", async (route) => {
    if (mock.connection.state === "connected") {
      const keys = queuedKeys || Array(3).fill(keyDown);
      queuedKeys = null;
      for (const down of keys) {
        sequence++;
        const phase = sequence * 0.035;
        const yaw = 0.3 * Math.sin(phase),
          pitch = 0.22 * Math.sin(phase * 1.4);
        const cy = Math.cos(yaw / 2),
          sy = Math.sin(yaw / 2),
          cp = Math.cos(pitch / 2),
          sp = Math.sin(pitch / 2);
        const q = [cy * cp, -sy * sp, cy * sp, sy * cp];
        const p = fixedPosition || [
          0.2 * Math.sin(phase),
          0.16 * Math.sin(phase * 1.3),
          0.18 * Math.cos(phase * 0.8),
        ];
        worldPoints.add(
          JSON.stringify(
            p.map((value) => Math.round(value * 1000000) / 1000000),
          ),
        );
        mock.pen_samples.push({
          seq: sequence,
          t: 100000 + sequence * 20,
          q,
          p,
          key_down: down,
        });
        mock.pose.quaternion = q;
        mock.pose.position_m = p;
      }
      mock.pen_samples = mock.pen_samples.slice(-256);
      mock.pen_cursor = sequence;
      mock.key = { available: keyDown !== null, down: keyDown };
    }
    responses++;
    await route.fulfill({ json: mock });
  });
  async function polls(count = 2) {
    for (let index = 0; index < count; index++) {
      await page.waitForResponse((response) =>
        response.url().includes("/api/state"),
      );
      await delay(30);
    }
  }
  async function holdAndRelease() {
    const count = (await draft(page)).strokes.length;
    keyDown = true;
    await recording(page, true);
    await polls(4);
    keyDown = false;
    await recording(page, false);
    return waitDraft(page, (value) => value.strokes.length === count + 1);
  }
  await polls();
  assert.equal(
    await page.locator("#pen-demo-key").isHidden(),
    true,
    "Real board mode must not display a mouse drawing trigger",
  );
  await page.locator('[data-pen-color="#397fc5"]').click();
  await setRange(page, "#pen-width", 8);
  await setRange(page, "#pen-sensitivity", 1.7);
  await polls();
  const twoStrokes = await holdAndRelease();
  const blueStroke = twoStrokes.strokes[1];
  assert.equal(blueStroke.color, "#397fc5");
  assert.equal(blueStroke.width, 8);
  assert.equal(twoStrokes.sensitivity, 1.7);
  assert.ok(blueStroke.points.length > 10);
  assert.ok(
    blueStroke.points.every(
      (point) => point.length === 3 && point.every(Number.isFinite),
    ),
  );
  assert.ok(
    blueStroke.points.every((point) => worldPoints.has(JSON.stringify(point))),
    "Camera scale must not change world XYZ or add an orientation tip offset",
  );
  for (let axis = 0; axis < 3; axis++) {
    const values = blueStroke.points.map((point) => point[axis]);
    assert.ok(
      Math.max(...values) - Math.min(...values) > 0.01,
      `Stroke must vary along XYZ axis ${axis}`,
    );
  }

  fixedPosition = [...mock.pose.position_m];
  const rotationOnly = await holdAndRelease();
  assert.equal(
    rotationOnly.strokes.at(-1).points.length,
    1,
    "Changing quaternion with fixed world position must not draw a line",
  );
  fixedPosition = null;
  await page.locator("#pen-undo").click();
  await waitDraft(page, (value) => value.strokes.length === 2);
  await polls();

  queuedKeys = [false, true, true, true, false];
  await polls();
  const quickDraft = await waitDraft(
    page,
    (value) => value.strokes.length === 3,
  );
  assert.ok(
    quickDraft.strokes[2].points.length >= 2,
    "A full KEY press/release between browser polls must survive sample batching",
  );
  await recording(page, false);
  await page.locator("#pen-undo").click();
  await waitDraft(page, (value) => value.strokes.length === 2);
  await page.locator("#pen-redo").click();
  assert.deepEqual(
    (await waitDraft(page, (value) => value.strokes.length === 3)).strokes,
    quickDraft.strokes,
  );

  await page.locator("#pen-center").click();
  await delay(350);
  const beforeOrbit = await canvas.screenshot();
  await dragCanvas(page, canvas);
  const afterOrbit = await canvas.screenshot();
  const orbitChanges = changedPixels(beforeOrbit, afterOrbit);
  assert.ok(
    orbitChanges > 200,
    `Mouse orbit must change the 3D view: ${orbitChanges}`,
  );
  const box = await canvas.boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.wheel(0, -400);
  await delay(350);
  const zoomChanges = changedPixels(afterOrbit, await canvas.screenshot());
  assert.ok(
    zoomChanges > 200,
    `Mouse wheel must change the camera distance: ${zoomChanges}`,
  );
  assert.deepEqual(
    (await draft(page)).strokes,
    quickDraft.strokes,
    "Camera interaction must not draw",
  );
  await page.locator("#pen-center").click();

  const exportedPng = await downloadFile(page, "#pen-export");
  assert.match(exportedPng.name, /\.png$/);
  const png = PNG.sync.read(exportedPng.data);
  assert.equal(png.width, 1600);
  assert.equal(png.height, 1000);
  const pixels = countPixels(exportedPng.data);
  assert.ok(
    pixels.ink > 200 && pixels.blue > 50,
    `PNG must contain colored 3D ink: ${JSON.stringify(pixels)}`,
  );
  const exportedJson = await downloadFile(page, "#pen-export-json");
  assert.match(exportedJson.name, /\.json$/);
  const geometry = JSON.parse(exportedJson.data.toString("utf8"));
  assert.equal(geometry.version, 4);
  assert.equal(geometry.dimensions, 3);
  assert.equal(geometry.units, "m");
  assert.equal(geometry.reference_frame.up_axis, "z");
  assert.equal(geometry.reference_frame.axes, "gravity-aligned");
  assert.equal("tip_length_m" in geometry, false);
  assert.deepEqual(geometry.strokes, quickDraft.strokes);

  const savedDraft = await draft(page);
  await page.reload();
  await page.locator('[data-tab="pen"]').click();
  await recording(page, false);
  await page.waitForFunction(() =>
    document.querySelector("#pen-stroke-count").textContent.startsWith("3 笔"),
  );
  assert.deepEqual(
    (await draft(page)).strokes,
    savedDraft.strokes,
    "Reload must restore XYZ ink without recording",
  );
  assert.equal(await page.locator("#pen-width").inputValue(), "8");
  assert.equal(await page.locator("#pen-sensitivity").inputValue(), "1.7");
  await noPageOverflow(page);
  await page.screenshot({
    path: path.join(screenshots, "desktop-air-pen.png"),
    fullPage: true,
  });
  const canvasPixels = countPixels(await canvas.screenshot());
  assert.ok(
    canvasPixels.blue > 30,
    "Desktop WebGL scene must render colored strokes",
  );
  await page.setViewportSize({ width: 390, height: 844 });
  await noPageOverflow(page);
  await page.screenshot({
    path: path.join(screenshots, "mobile-air-pen.png"),
    fullPage: true,
  });
  const mobilePixels = countPixels(await canvas.screenshot());
  assert.ok(
    mobilePixels.blue > 5,
    "Mobile WebGL scene must render colored strokes",
  );
  await page.setViewportSize({ width: 1440, height: 1100 });

  await page.locator('[data-tab="library"]').click();
  keyDown = true;
  await polls();
  await page.locator('[data-tab="pen"]').click();
  await polls(3);
  await recording(page, false);
  assert.deepEqual(
    (await draft(page)).strokes,
    savedDraft.strokes,
    "Entering while KEY is already held must wait for release",
  );
  keyDown = false;
  await polls();
  keyDown = true;
  await recording(page, true);
  await polls();
  await page.locator('[data-tab="library"]').click();
  await delay(150);
  const tabPaused = await draft(page);
  await polls();
  await page.locator('[data-tab="pen"]').click();
  await polls();
  await recording(page, false);
  assert.deepEqual(
    (await draft(page)).strokes,
    tabPaused.strokes,
    "Leaving the tab must finish ink and not resume while KEY remains held",
  );

  keyDown = false;
  await polls();
  keyDown = true;
  await recording(page, true);
  await polls();
  mock.connection.state = "disconnected";
  await recording(page, false);
  const disconnected = await draft(page);
  await polls();
  assert.deepEqual(
    (await draft(page)).strokes,
    disconnected.strokes,
    "Disconnect must stop recording",
  );
  mock.connection.state = "connected";
  mock.connection.epoch++;
  mock.pen_epoch++;
  mock.pen_samples = [];
  keyDown = null;
  await polls(3);
  await recording(page, false);
  assert.deepEqual(
    (await draft(page)).strokes,
    disconnected.strokes,
    "Legacy firmware without KEY state must never draw",
  );
  assert.ok((await page.locator("#pen-detail").textContent()).includes("KEY"));

  const beforeClear = (await draft(page)).strokes;
  await page.locator("#pen-clear").click();
  await page.locator("#pen-clear-dialog").waitFor({ state: "visible" });
  await page.locator("#pen-clear-cancel").click();
  assert.deepEqual((await draft(page)).strokes, beforeClear);
  await page.locator("#pen-clear").click();
  await page.locator("#pen-clear-confirm").click();
  await waitDraft(page, (value) => value.strokes.length === 0);
  assert.equal(await page.locator("#pen-export").isDisabled(), true);
  assert.equal(await page.locator("#pen-export-json").isDisabled(), true);
  assert.equal(await page.locator("#pen-undo").isDisabled(), true);
  assert.deepEqual(errors, [], "Browser console errors");
  console.log(
    JSON.stringify(
      {
        result: "passed",
        demoPoints: liveDraft.strokes[0].points.length,
        xyzPoints: blueStroke.points.length,
        pixels,
        canvasPixels,
        mobilePixels,
        orbitChanges,
        zoomChanges,
        mockResponses: responses,
        screenshots,
        dataDir,
      },
      null,
      2,
    ),
  );
})()
  .catch(async (error) => {
    console.error(error);
    if (diagnosticPage)
      console.error(
        await diagnosticPage
          .evaluate(() => ({
            status: document.querySelector("#pen-status")?.textContent,
            detail: document.querySelector("#pen-detail")?.textContent,
            connection:
              document.querySelector("#connection-status")?.textContent,
          }))
          .catch(() => ({})),
      );
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
