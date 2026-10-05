"""Relative IMU pose for the local viewer, with no board calibration prerequisite.

Fusion uses a right-handed, gravity-aligned NWU frame (Z up), SI input, and
body-to-world quaternions in w,x,y,z order.  The displayed frame is the board
orientation at the first sample or latest zero(): q_relative = q_origin^-1 * q.
All displayed vectors use that same frame.  Consequently, zeroing a tilted
board also tilts the displayed reference axes relative to gravity.

Six-axis IMUs cannot observe absolute yaw or position.  Translation is a
short-term estimate from gravity-compensated acceleration; stationary updates
limit drift but cannot distinguish constant velocity from rest.
"""

from collections import deque
import time

import imufusion
import numpy as np


GRAVITY = 9.80665
SAMPLE_RATE = 200
MAX_SAMPLE_GAP_S = 0.1
MAX_WALL_GAP_S = 0.5
MAX_SPEED_M_S = 10.0
MAX_DISPLACEMENT_M = 20.0
UINT32_MASK = 0xFFFFFFFF


def _multiply(left, right):
    w, x, y, z = left
    a, b, c, d = right
    return np.array((w*a-x*b-y*c-z*d, w*b+x*a+y*d-z*c,
                     w*c-x*d+y*a+z*b, w*d+x*c-y*b+z*a))


def _normalise_quaternion(value):
    value = np.asarray(value, dtype=float)
    return value / np.linalg.norm(value)


def _gravity_quaternion(acceleration):
    # Shortest rotation from measured body gravity to world +Z avoids the
    # large startup acceleration artefact of initializing a tilted IMU flat.
    direction = acceleration / np.linalg.norm(acceleration)
    if direction[2] < -0.999999:
        return np.array((0.0, 1.0, 0.0, 0.0))
    return _normalise_quaternion((1.0 + direction[2], direction[1],
                                  -direction[0], 0.0))


class PoseEstimator:
    """Consume 200 Hz RAW records; callers provide serialization if shared."""

    def __init__(self, clock=None):
        self._clock = clock or time.monotonic
        self.reset()

    def reset(self):
        """Discard all sensor/session history on reconnect or device reboot."""
        self._ahrs = imufusion.Ahrs()
        self._fusion_settings = imufusion.Settings(
            imufusion.CONVENTION_NWU, 0.35, 2000, 8, 0, 5 * SAMPLE_RATE)
        self._ahrs.settings = self._fusion_settings
        self._offset = imufusion.Offset(SAMPLE_RATE)
        self._quaternion = np.array((1.0, 0.0, 0.0, 0.0))
        self._origin = self._quaternion.copy()
        self._origin_inverse_rotation = np.eye(3)
        self._position = np.zeros(3)
        self._velocity = np.zeros(3)
        self._filtered_linear = np.zeros(3)
        self._world_linear = np.zeros(3)
        self._linear = np.zeros(3)
        self._previous_linear = np.zeros(3)
        self._trail = deque(maxlen=160)
        self._last_t = None
        self._last_seq = None
        self._last_wall = None
        self._elapsed = 0.0
        self._startup_elapsed = 0.0
        self._trail_elapsed = 0.0
        self._still_elapsed = 0.0
        self._stationary = False
        self._available = False
        self._position_limited = False
        self._status = "waiting"

    def zero(self):
        """Keep Fusion running and set the current pose as the relative origin."""
        self._origin = self._quaternion.copy()
        self._origin_inverse_rotation = np.asarray(
            imufusion.Quaternion(self._origin).to_matrix(), dtype=float).T
        self._position.fill(0)
        self._velocity.fill(0)
        self._filtered_linear.fill(0)
        self._linear.fill(0)
        self._previous_linear.fill(0)
        self._trail.clear()
        self._trail_elapsed = 0.0
        self._elapsed = 0.0
        self._position_limited = False
        if self._available:
            self._status = "stationary" if self._stationary else "tracking"

    def _gap(self):
        self._velocity.fill(0)
        self._world_linear.fill(0)
        self._filtered_linear.fill(0)
        self._linear.fill(0)
        self._previous_linear.fill(0)
        self._still_elapsed = 0.0
        self._stationary = False
        self._status = "gap" if self._available else "waiting"

    @staticmethod
    def _record(record):
        try:
            tick = record["t"]
            seq = record.get("seq")
            if isinstance(tick, bool) or int(tick) != tick or not 0 <= tick <= UINT32_MASK:
                return None
            if seq is not None and (isinstance(seq, bool) or int(seq) != seq or
                                    not 0 <= seq <= UINT32_MASK):
                return None
            acceleration = np.array([record[key] for key in ("ax", "ay", "az")], dtype=float)
            gyroscope = np.array([record[key] for key in ("gx", "gy", "gz")], dtype=float)
            if (not np.all(np.isfinite(acceleration)) or not np.all(np.isfinite(gyroscope)) or
                    np.any(np.abs(acceleration) > 200) or np.any(np.abs(gyroscope) > 40)):
                return None
            return int(tick), None if seq is None else int(seq), acceleration, gyroscope
        except (KeyError, TypeError, ValueError, OverflowError):
            return None

    def feed(self, record):
        """Return True for an updated orientation; input uses ms, m/s^2, rad/s."""
        parsed = self._record(record)
        if parsed is None:
            self._gap()
            return
        tick, seq, acceleration, gyroscope = parsed
        now = self._clock()
        if self._last_t is not None:
            delta_ms = (tick - self._last_t) & UINT32_MASK
            delta_seq = None if seq is None or self._last_seq is None else (seq - self._last_seq) & UINT32_MASK
            # Unsigned arithmetic accepts the normal counter wrap but rejects
            # duplicates and older packets without moving the time baseline.
            if delta_ms == 0 or delta_ms >= 0x80000000 or (
                    delta_seq is not None and (delta_seq == 0 or delta_seq >= 0x80000000)):
                return
            dt = delta_ms / 1000.0
            self._last_t, self._last_seq = tick, seq
            wall_gap = now - self._last_wall > MAX_WALL_GAP_S
            self._last_wall = now
            if dt > MAX_SAMPLE_GAP_S or wall_gap:
                self._gap()
                return
        else:
            # Gravity can only initialize tilt when there is a useful vector.
            if not 0.5 * GRAVITY <= np.linalg.norm(acceleration) <= 1.5 * GRAVITY:
                return
            self._quaternion = _gravity_quaternion(acceleration)
            self._ahrs.quaternion = imufusion.Quaternion(self._quaternion)
            # Tilt is already seeded from gravity. Finish Fusion's startup at
            # zero elapsed time; its default startup otherwise forces yaw to
            # zero for three seconds in the no-magnetometer update method.
            self._ahrs.settings = imufusion.Settings(
                imufusion.CONVENTION_NWU, 0, 2000, 8, 0, 5 * SAMPLE_RATE)
            self._ahrs.update_no_magnetometer(np.zeros(3), acceleration / GRAVITY, 0.0)
            self._ahrs.settings = self._fusion_settings
            self._last_t, self._last_seq, self._last_wall = tick, seq, now
            self._available = True
            self.zero()
            dt = 0.0

        corrected_gyro = self._offset.update(np.rad2deg(gyroscope))
        self._ahrs.update_no_magnetometer(corrected_gyro, acceleration / GRAVITY, dt)
        self._quaternion = _normalise_quaternion(self._ahrs.quaternion.wxyz)
        earth_linear = np.asarray(self._ahrs.earth_acceleration, dtype=float) * GRAVITY
        if not np.all(np.isfinite(earth_linear)) or not np.all(np.isfinite(self._quaternion)):
            self.reset()
            return

        self._elapsed += dt
        self._startup_elapsed = min(0.25, self._startup_elapsed + dt)
        alpha = dt / (0.035 + dt)
        # Keep the world's measured residual for pen bias estimation, even
        # when the spatial viewer suppresses stationary acceleration.
        self._world_linear += alpha * (earth_linear - self._world_linear)
        self._filtered_linear += alpha * (earth_linear - self._filtered_linear)
        self._linear = self._filtered_linear.copy()
        if np.linalg.norm(self._linear) < 0.12:
            self._linear.fill(0)

        quiet = (np.linalg.norm(gyroscope) < 0.055 and
                 abs(np.linalg.norm(acceleration) - GRAVITY) < 0.16 and
                 np.linalg.norm(earth_linear) < 0.22)
        self._still_elapsed = min(0.2, self._still_elapsed + dt) if quiet else 0.0
        self._stationary = self._still_elapsed >= 0.2
        if self._position_limited or self._stationary or self._startup_elapsed < 0.25:
            self._velocity.fill(0)
            self._previous_linear.fill(0)
            if self._stationary:
                self._filtered_linear.fill(0)
                self._linear.fill(0)
        else:
            next_velocity = self._velocity + (self._previous_linear + self._linear) * (0.5 * dt)
            next_position = self._position + (self._velocity + next_velocity) * (0.5 * dt)
            if (not np.all(np.isfinite(next_velocity)) or not np.all(np.isfinite(next_position)) or
                    np.linalg.norm(next_velocity) > MAX_SPEED_M_S or
                    np.linalg.norm(next_position) > MAX_DISPLACEMENT_M):
                # Handheld dead reckoning has left its useful range. Freeze
                # translation until explicit zero; never silently clip a path.
                self._position_limited = True
                self._gap()
            else:
                self._position = next_position
                self._velocity = next_velocity
                self._previous_linear = self._linear.copy()

        self._status = "gap" if self._position_limited else "stationary" if self._stationary else "tracking"
        self._trail_elapsed = min(0.1, self._trail_elapsed + dt) if self._position_limited else self._trail_elapsed + dt
        if not self._position_limited and self._trail_elapsed + 1e-9 >= 0.1:
            self._trail_elapsed %= 0.1
            self._trail.append(self._relative_vector(self._position))
        return True

    def _relative_vector(self, vector):
        return [float(value) for value in self._origin_inverse_rotation @ vector]

    def world_motion(self):
        """Gravity-aligned Fusion frame, independent of the display's tilted zero."""
        return {
            "quaternion": [float(value) for value in self._quaternion],
            "acceleration_m_s2": [float(value) for value in self._world_linear],
            "stationary": self._stationary,
            "timestamp_ms": self._last_t,
        }

    def snapshot(self):
        if self._available and self._clock() - self._last_wall > MAX_WALL_GAP_S:
            self._gap()
        inverse_origin = self._origin * np.array((1.0, -1.0, -1.0, -1.0))
        quaternion = _normalise_quaternion(_multiply(inverse_origin, self._quaternion))
        angles = imufusion.Quaternion(quaternion).to_euler()
        return {
            "available": self._available,
            "quaternion": [float(value) for value in quaternion],
            "euler_deg": dict(zip(("roll", "pitch", "yaw"), (float(value) for value in angles))),
            "position_m": self._relative_vector(self._position),
            "velocity_m_s": self._relative_vector(self._velocity),
            "linear_accel_m_s2": self._relative_vector(self._linear),
            "stationary": self._stationary,
            "position_limited": self._position_limited,
            "elapsed_s": float(self._elapsed),
            "status": self._status,
            "trail": [list(point) for point in self._trail],
            "timestamp_ms": self._last_t,
        }
