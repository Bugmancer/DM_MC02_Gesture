"""Synthetic SI-unit IMU streams; no board, browser, or timing sleeps needed."""

import json
import math
import unittest

import imufusion
import numpy as np

from pose import GRAVITY, PoseEstimator


class PoseTests(unittest.TestCase):
    def setUp(self):
        self.wall = 0.0
        self.pose = PoseEstimator(clock=lambda: self.wall)
        self.seq = 0
        self.tick = 0

    def sample(self, acceleration=(0, 0, GRAVITY), gyroscope=(0, 0, 0), count=1):
        for _ in range(count):
            self.seq = (self.seq + 1) & 0xFFFFFFFF
            self.tick = (self.tick + 5) & 0xFFFFFFFF
            self.pose.feed(dict(zip(("seq", "t", "ax", "ay", "az", "gx", "gy", "gz"),
                                    (self.seq, self.tick, *acceleration, *gyroscope))))
        return self.pose.snapshot()

    def assert_gravity_aligned(self, quaternion, body_gravity):
        rotation = np.asarray(imufusion.Quaternion(np.asarray(quaternion)).to_matrix())
        np.testing.assert_allclose(rotation @ body_gravity, (0, 0, GRAVITY), atol=1e-5)

    def test_flat_stationary_has_no_translation(self):
        self.assertFalse(self.pose.snapshot()["available"])
        pose = self.sample(count=1000)
        self.assertTrue(pose["available"])
        self.assertTrue(pose["stationary"])
        self.assertEqual(pose["status"], "stationary")
        np.testing.assert_allclose(pose["position_m"], 0, atol=1e-8)
        np.testing.assert_allclose(pose["velocity_m_s"], 0, atol=1e-8)
        np.testing.assert_allclose(pose["quaternion"], (1, 0, 0, 0), atol=1e-8)

    def test_tilted_and_upside_down_start_keep_world_z_aligned_with_gravity(self):
        for gravity in ((0, GRAVITY / math.sqrt(2), GRAVITY / math.sqrt(2)),
                        (-GRAVITY / 2, 0, GRAVITY * math.sqrt(3) / 2),
                        (0, 0, -GRAVITY)):
            with self.subTest(gravity=gravity):
                self.pose.reset()
                pose = self.sample(acceleration=gravity)
                self.assertTrue(pose["available"])
                self.assert_gravity_aligned(pose["quaternion"], gravity)
                pose = self.sample(acceleration=gravity, count=1000)
                self.assertTrue(pose["stationary"])
                self.assert_gravity_aligned(pose["quaternion"], gravity)
                np.testing.assert_allclose(pose["linear_accel_m_s2"], 0, atol=1e-5)
                np.testing.assert_allclose(pose["position_m"], 0)

    def test_zero_preserves_heading_and_orientation_continuity(self):
        self.sample(count=800)
        before = self.sample(gyroscope=(0, 0, math.pi / 2), count=200)
        self.assertAlmostEqual(before["euler_deg"]["yaw"], 90, delta=0.1)
        self.pose.set_translation((1, 2, 3), (.1, .2, .3), [(0, 0, 0), (1, 2, 3)])
        self.pose.zero()
        zero = self.pose.snapshot()
        np.testing.assert_allclose(zero["quaternion"], before["quaternion"], atol=1e-8)
        np.testing.assert_allclose(zero["position_m"], 0)
        np.testing.assert_allclose(zero["velocity_m_s"], 0)
        self.assertEqual(zero["trail"], [])
        self.assertEqual(zero["elapsed_s"], 0)
        pose = self.sample(gyroscope=(0, 0, math.pi / 2), count=100)
        self.assertAlmostEqual(pose["euler_deg"]["yaw"], 135, delta=0.1)

    def test_yaw_is_visible_immediately_without_calibration_wait(self):
        self.sample()
        pose = self.sample(gyroscope=(0, 0, math.pi / 2), count=200)
        self.assertAlmostEqual(pose["euler_deg"]["yaw"], 90, delta=0.1)

    def test_physical_roll_sign_from_rotating_gravity(self):
        self.sample(count=800)
        rate = math.pi / 4
        for index in range(1, 201):
            angle = rate * index * .005
            pose = self.sample(acceleration=(0, GRAVITY * math.sin(angle), GRAVITY * math.cos(angle)),
                               gyroscope=(rate, 0, 0))
        self.assertAlmostEqual(pose["euler_deg"]["roll"], 45, delta=.5)
        self.assertAlmostEqual(pose["euler_deg"]["pitch"], 0, delta=.1)
        np.testing.assert_allclose(pose["position_m"], 0)

    def test_tilted_zero_keeps_display_and_motion_in_same_gravity_frame(self):
        gravity = (0, GRAVITY / math.sqrt(2), GRAVITY / math.sqrt(2))
        before = self.sample(acceleration=gravity, count=200)
        self.pose.zero()
        zero = self.pose.snapshot()
        np.testing.assert_allclose(zero["quaternion"], before["quaternion"])
        self.assertAlmostEqual(zero["euler_deg"]["roll"], 45, delta=.001)
        acceleration = (0, (GRAVITY + 1) / math.sqrt(2), (GRAVITY + 1) / math.sqrt(2))
        local = self.sample(acceleration=acceleration, count=200)
        world = self.pose.world_motion()
        np.testing.assert_allclose(world["quaternion"], local["quaternion"])
        np.testing.assert_allclose(world["acceleration_m_s2"], local["linear_accel_m_s2"])
        np.testing.assert_allclose(world["acceleration_m_s2"], (0, 0, 1), atol=.001)
        np.testing.assert_allclose(world["body_acceleration_m_s2"], acceleration)

    def test_world_motion_exposes_bias_corrected_gyroscope_in_radians(self):
        class CorrectedOffset:
            def update(self, degrees):
                self.received = degrees.copy()
                return np.array((12., -6., 30.))

        self.sample()
        offset = CorrectedOffset()
        self.pose._offset = offset
        self.sample(gyroscope=(.25, -.5, 1.))
        world = self.pose.world_motion()
        np.testing.assert_allclose(offset.received, np.rad2deg((.25, -.5, 1.)))
        np.testing.assert_allclose(world["gyroscope_rad_s"], np.deg2rad((12, -6, 30)))
        np.testing.assert_allclose(world["body_acceleration_m_s2"], (0, 0, GRAVITY))
        self.assertTrue(world["available"])
        self.assertEqual(world["timestamp_ms"], self.tick)

    def test_small_stationary_residual_is_preserved_without_deadband(self):
        local = self.sample(acceleration=(0, 0, GRAVITY + .01), count=800)
        self.assertTrue(local["stationary"])
        self.assertAlmostEqual(local["linear_accel_m_s2"][2], .01, delta=1e-5)
        world = self.pose.world_motion()
        self.assertAlmostEqual(world["acceleration_m_s2"][2], .01, delta=1e-5)
        self.pose.zero()
        np.testing.assert_allclose(self.pose.world_motion()["acceleration_m_s2"],
                                   world["acceleration_m_s2"])

    def test_attitude_feed_never_integrates_or_limits_external_translation(self):
        self.sample(count=200)
        self.pose.set_translation((100, -20, 8), (12, -3, 4))
        pose = self.sample(acceleration=(0, 0, GRAVITY + 50), count=1400)
        np.testing.assert_allclose(pose["position_m"], (100, -20, 8))
        np.testing.assert_allclose(pose["velocity_m_s"], (12, -3, 4))
        self.assertFalse(pose["position_limited"])
        self.assertEqual(pose["status"], "tracking")
        pose = self.sample(count=200)
        self.assertTrue(pose["stationary"])
        np.testing.assert_allclose(pose["velocity_m_s"], (12, -3, 4))
        json.dumps(pose, allow_nan=False)

    def test_feed_alone_produces_no_translation_or_trail(self):
        self.sample(count=200)
        pose = self.sample(acceleration=(0, 0, GRAVITY + 1), count=2000)
        np.testing.assert_allclose(pose["position_m"], 0)
        np.testing.assert_allclose(pose["velocity_m_s"], 0)
        self.assertEqual(pose["trail"], [])

    def test_translation_setter_copies_world_vectors_and_replacement_trail(self):
        gravity = (0, GRAVITY / math.sqrt(2), GRAVITY / math.sqrt(2))
        self.sample(acceleration=gravity, count=200)
        position = [1, 2, 3]
        velocity = [.1, .2, .3]
        trail = [[0, 0, 0], [1, 2, 3]]
        self.assertTrue(self.pose.set_translation(position, velocity, trail))
        position[0] = velocity[0] = trail[0][0] = 900
        snapshot = self.pose.snapshot()
        self.assertEqual(snapshot["position_m"], [1, 2, 3])
        self.assertEqual(snapshot["velocity_m_s"], [.1, .2, .3])
        self.assertEqual(snapshot["trail"], [[0, 0, 0], [1, 2, 3]])
        snapshot["position_m"][0] = snapshot["trail"][0][0] = 700
        self.assertEqual(self.pose.snapshot()["position_m"], [1, 2, 3])
        self.assertEqual(self.pose.snapshot()["trail"][0], [0, 0, 0])
        self.assertTrue(self.pose.set_translation((0, 0, 0), (0, 0, 0), []))
        self.assertEqual(self.pose.snapshot()["trail"], [])

    def test_invalid_translation_is_rejected_atomically(self):
        self.sample(count=200)
        self.pose.set_translation((1, 2, 3), (.1, .2, .3), [(1, 2, 3)])
        before = self.pose.snapshot()
        for position, velocity, trail in (
                ((1, 2), (0, 0, 0), None),
                ((1, 2, 3), (0, float("nan"), 0), None),
                ((float("inf"), 2, 3), (0, 0, 0), None),
                ((1, 2, 3), (0, 0, 0), [(0, 0)]),
                ((1, 2, 3), (0, 0, 0), [(0, float("inf"), 0)]),
                ((1, 2, 3), (0, 0, 0), "invalid")):
            with self.subTest(position=position, velocity=velocity, trail=trail):
                self.assertFalse(self.pose.set_translation(position, velocity, trail))
                self.assertEqual(before, self.pose.snapshot())

    def test_translation_trail_is_bounded_and_sample_time_decimated(self):
        for index in range(4001):
            self.sample()
            self.pose.set_translation((index, 0, 0), (0, 0, 0))
        trail = self.pose.snapshot()["trail"]
        self.assertEqual(len(trail), 160)
        self.assertTrue(all(b[0] - a[0] == 20 for a, b in zip(trail, trail[1:])))
        self.pose.set_translation((99999, 0, 0), (0, 0, 0))
        self.assertEqual(self.pose.snapshot()["trail"], trail)
        self.pose.set_translation((0, 0, 0), (0, 0, 0), [(i, 0, 0) for i in range(200)])
        trail = self.pose.snapshot()["trail"]
        self.assertEqual(len(trail), 160)
        self.assertEqual(trail[0], [40, 0, 0])

    def test_uint32_wrap_duplicates_and_old_packets(self):
        self.tick = 0xFFFFFFF0
        self.seq = 0xFFFFFFF0
        pose = self.sample(count=100)
        self.assertAlmostEqual(pose["elapsed_s"], .495)
        self.assertLess(pose["timestamp_ms"], 1000)
        before = self.pose.snapshot()
        for tick, seq in ((self.tick, self.seq), (self.tick + 1, self.seq), (self.tick - 5, self.seq - 1)):
            self.pose.feed({"seq": seq, "t": tick, "ax": 0, "ay": 0, "az": GRAVITY + 5,
                            "gx": 0, "gy": 0, "gz": 1})
        self.assertEqual(before, self.pose.snapshot())

    def test_gaps_preserve_position_and_do_not_rotate_over_missing_time(self):
        self.sample(count=200)
        self.pose.set_translation((1, 2, 3), (.1, .2, .3))
        before = self.pose.snapshot()
        self.tick += 10000
        pose = self.sample(gyroscope=(0, 0, math.pi / 2))
        self.assertEqual(pose["status"], "gap")
        self.assertEqual(pose["position_m"], before["position_m"])
        self.assertEqual(pose["quaternion"], before["quaternion"])
        self.assertEqual(pose["elapsed_s"], before["elapsed_s"])
        np.testing.assert_allclose(pose["velocity_m_s"], 0)
        self.assertFalse(self.pose.world_motion()["available"])
        pose = self.sample(count=100)
        self.assertEqual(pose["status"], "stationary")
        self.wall = 1
        motion = self.pose.world_motion()
        self.assertEqual(motion["status"], "gap")
        self.assertFalse(motion["stationary"])
        self.assertFalse(motion["available"])
        self.assertEqual(self.sample()["status"], "gap")

    def test_invalid_samples_are_finite_and_do_not_move(self):
        self.sample(count=200)
        for field, value in (("ax", float("nan")), ("gy", float("inf")), ("az", 10000),
                             ("gx", -1000), ("t", -1), ("t", float("nan")),
                             ("seq", float("inf")), ("ax", [1]), ("t", True)):
            with self.subTest(field=field, value=value):
                record = {"seq": self.seq + 1, "t": self.tick + 5, "ax": 0, "ay": 0,
                          "az": GRAVITY, "gx": 0, "gy": 0, "gz": 0}
                record[field] = value
                self.assertFalse(self.pose.feed(record))
                pose = self.pose.snapshot()
                self.assertEqual(pose["status"], "gap")
                np.testing.assert_allclose(pose["position_m"], 0)
                json.dumps(pose, allow_nan=False)
                json.dumps(self.pose.world_motion(), allow_nan=False)

    def test_initial_gravity_requires_a_valid_vector(self):
        self.assertFalse(self.sample(acceleration=(0, 0, 0))["available"])
        self.assertFalse(self.sample(acceleration=(0, 0, 100))["available"])
        self.assertTrue(self.sample()["available"])

    def test_zero_during_gap_does_not_make_stale_sensor_data_available(self):
        self.sample(count=200)
        self.wall = 1
        self.pose.snapshot()
        self.pose.zero()
        self.assertEqual(self.pose.snapshot()["status"], "gap")
        self.assertFalse(self.pose.world_motion()["available"])

    def test_reset_and_waiting_zero_do_not_reuse_a_previous_session(self):
        self.sample(count=200)
        self.sample(gyroscope=(0, 0, 1), count=100)
        self.pose.set_translation((1, 2, 3), (4, 5, 6))
        self.pose.reset()
        self.pose.zero()
        pose = self.pose.snapshot()
        self.assertFalse(pose["available"])
        self.assertEqual(pose["status"], "waiting")
        self.assertIsNone(pose["timestamp_ms"])
        self.assertEqual(pose["trail"], [])
        np.testing.assert_allclose(pose["position_m"], 0)
        self.tick = self.seq = 0
        pose = self.sample(count=100)
        self.assertTrue(pose["stationary"])
        np.testing.assert_allclose(pose["quaternion"], (1, 0, 0, 0), atol=1e-8)


if __name__ == "__main__":
    unittest.main()
