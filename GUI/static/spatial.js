import * as THREE from "./vendor/three.module.js";
import { OrbitControls } from "./vendor/OrbitControls.js";

const FRAME_COLOR = 0xf4f7f5;
const AXIS_COLORS = [0xc96353, 0x1a987c, 0x5b84c3];
const MAX_TRAIL_POINTS = 2048;

function scenePosition(values, target) {
  return target.set(values[0], values[2], -values[1]);
}

function finiteVector(values, length) {
  return (
    Array.isArray(values) &&
    values.length === length &&
    values.every(Number.isFinite)
  );
}

function axisLabel(text, color) {
  const canvas = document.createElement("canvas");
  canvas.width = canvas.height = 64;
  const context = canvas.getContext("2d");
  context.fillStyle = `#${color.toString(16).padStart(6, "0")}`;
  context.font = "600 32px sans-serif";
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.fillText(text, 32, 32);
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  const sprite = new THREE.Sprite(
    new THREE.SpriteMaterial({ map: texture, depthTest: false }),
  );
  sprite.scale.setScalar(0.023);
  return sprite;
}

function makeWorldAxes() {
  const group = new THREE.Group();
  group.name = "fixed-world-axes";
  const axes = [
    new THREE.Vector3(1, 0, 0),
    new THREE.Vector3(0, 0, -1),
    new THREE.Vector3(0, 1, 0),
  ];
  const lengths = [0.15, 0.15, 0.15];
  axes.forEach((direction, index) => {
    const origin = new THREE.Vector3();
    group.add(
      new THREE.ArrowHelper(
        direction,
        origin,
        lengths[index],
        AXIS_COLORS[index],
        0.011,
        0.006,
      ),
    );
    const label = axisLabel(["X", "Y", "Z"][index], AXIS_COLORS[index]);
    label.position
      .copy(direction)
      .multiplyScalar(lengths[index] + 0.012)
      .add(origin);
    group.add(label);
  });
  return group;
}

export function createSpatialView(container) {
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(FRAME_COLOR);
  scene.fog = new THREE.Fog(FRAME_COLOR, 0.65, 1.8);
  const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.domElement.setAttribute(
    "aria-label",
    "固定参考坐标系中的三维轨迹，可旋转和缩放",
  );
  renderer.domElement.setAttribute("role", "img");
  container.append(renderer.domElement);

  const camera = new THREE.PerspectiveCamera(35, 1, 0.005, 100);
  const controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.dampingFactor = 0.12;
  controls.minDistance = 0.1;
  controls.maxDistance = 3;
  controls.maxPolarAngle = Math.PI * 0.91;
  controls.enablePan = true;
  const cameraOffset = new THREE.Vector3(0.3, 0.3, 0.36);
  camera.position.copy(cameraOffset);
  controls.update();

  const grid = new THREE.GridHelper(1.2, 60, 0xd7e1db, 0xd7e1db);
  grid.position.y = -0.006;
  grid.material.transparent = true;
  grid.material.opacity = 0.65;
  scene.add(grid);
  const origin = new THREE.LineSegments(
    new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(-0.012, -0.005, 0),
      new THREE.Vector3(0.012, -0.005, 0),
      new THREE.Vector3(0, -0.005, -0.012),
      new THREE.Vector3(0, -0.005, 0.012),
    ]),
    new THREE.LineBasicMaterial({ color: 0x7b9587 }),
  );
  scene.add(origin);

  scene.add(makeWorldAxes());
  const positionMarker = new THREE.Mesh(
    new THREE.SphereGeometry(0.004, 16, 12),
    new THREE.MeshBasicMaterial({ color: 0x243632 }),
  );
  positionMarker.visible = false;
  scene.add(positionMarker);
  const trailPositions = new Float32Array(MAX_TRAIL_POINTS * 3);
  const trailGeometry = new THREE.BufferGeometry();
  trailGeometry.setAttribute(
    "position",
    new THREE.BufferAttribute(trailPositions, 3).setUsage(
      THREE.DynamicDrawUsage,
    ),
  );
  trailGeometry.setDrawRange(0, 0);
  const trail = new THREE.Line(
    trailGeometry,
    new THREE.LineBasicMaterial({
      color: 0x1c9c82,
      transparent: true,
      opacity: 0.75,
    }),
  );
  trail.frustumCulled = false;
  scene.add(trail);

  const scratchPosition = new THREE.Vector3();
  let latestPose = { available: false };
  let visible = false;
  let frameId = 0;
  let disposed = false;

  function applyPose() {
    const pose = latestPose;
    if (!pose.available || !finiteVector(pose.position_m, 3)) {
      positionMarker.visible = false;
      trailGeometry.setDrawRange(0, 0);
      return;
    }
    scenePosition(pose.position_m, positionMarker.position);
    positionMarker.visible = true;
    const points = (pose.trail || []).slice(-MAX_TRAIL_POINTS);
    let count = 0;
    for (const point of points) {
      if (!finiteVector(point, 3)) continue;
      scenePosition(point, scratchPosition).toArray(trailPositions, count * 3);
      count += 1;
    }
    trailGeometry.attributes.position.needsUpdate = true;
    trailGeometry.setDrawRange(0, count);
  }

  function resize() {
    if (!visible || disposed) return;
    const width = Math.max(1, container.clientWidth);
    const height = Math.max(1, container.clientHeight);
    renderer.setSize(width, height, false);
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
  }

  function draw() {
    frameId = 0;
    if (!visible || document.hidden || disposed) return;
    controls.update();
    renderer.render(scene, camera);
    frameId = requestAnimationFrame(draw);
  }

  function schedule() {
    if (visible && !document.hidden && !frameId && !disposed) {
      frameId = requestAnimationFrame(draw);
    }
  }

  function visibilityChanged() {
    if (document.hidden) {
      cancelAnimationFrame(frameId);
      frameId = 0;
    } else schedule();
  }

  const observer = new ResizeObserver(resize);
  observer.observe(container);
  document.addEventListener("visibilitychange", visibilityChanged);

  return {
    update(pose) {
      latestPose = pose || { available: false };
      if (visible) applyPose();
    },
    setVisible(nextVisible) {
      visible = Boolean(nextVisible);
      controls.enabled = visible;
      if (visible) {
        applyPose();
        resize();
        schedule();
      } else {
        cancelAnimationFrame(frameId);
        frameId = 0;
      }
    },
    resetCamera() {
      controls.target.set(0, 0, 0);
      camera.position.copy(cameraOffset);
      camera.zoom = 1;
      camera.updateProjectionMatrix();
      controls.update();
    },
    setTrailVisible(nextVisible) {
      trail.visible = Boolean(nextVisible);
    },
    dispose() {
      disposed = true;
      cancelAnimationFrame(frameId);
      observer.disconnect();
      document.removeEventListener("visibilitychange", visibilityChanged);
      controls.dispose();
      scene.traverse((object) => {
        object.geometry?.dispose();
        const materials = Array.isArray(object.material)
          ? object.material
          : [object.material];
        for (const material of materials) {
          material?.map?.dispose();
          material?.dispose();
        }
      });
      renderer.dispose();
      renderer.domElement.remove();
    },
  };
}
