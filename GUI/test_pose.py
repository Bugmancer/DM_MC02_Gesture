"""Synthetic SI-unit IMU streams; no board, browser, or timing sleeps needed."""

import json
import math
import unittest

import numpy as np

from pose import GRAVITY, MAX_DISPLACEMENT_M, MAX_SPEED_M_S, PoseEstimator


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

    def test_flat_stationary_has_no_translation(self):
        self.assertFalse(self.pose.snapshot()["available"])
        pose = self.sample(count=1000)
        self.assertTrue(pose["available"])
        self.assertTrue(pose["stationary"])
        self.assertEqual(pose["status"], "stationary")
        np.testing.assert_allclose(pose["position_m"], 0, atol=1e-8)
        np.testing.assert_allclose(pose["velocity_m_s"], 0, atol=1e-8)
        np.testing.assert_allclose(pose["quaternion"], (1, 0, 0, 0), atol=1e-8)

    def test_tilted_and_upside_down_start_without_false_motion(self):
        for acceleration in ((0, GRAVITY / math.sqrt(2), GRAVITY / math.sqrt(2)),
                             (-GRAVITY / 2, 0, GRAVITY * math.sqrt(3) / 2),
                             (0, 0, -GRAVITY)):
            self.pose.reset()
            pose = self.sample(acceleration=acceleration, count=1000)
            self.assertTrue(pose["stationary"])
            np.testing.assert_allclose(pose["position_m"], 0, atol=1e-6)
            np.testing.assert_allclose(pose["quaternion"], (1, 0, 0, 0), atol=1e-5)

    def test_known_positive_yaw_rotation_and_zero(self):
        self.sample(count=800)
        pose = self.sample(gyroscope=(0, 0, math.pi / 2), count=200)
        self.assertAlmostEqual(pose["euler_deg"]["yaw"], 90, delta=0.1)
        np.testing.assert_allclose(pose["quaternion"], (math.sqrt(.5), 0, 0, math.sqrt(.5)), atol=.002)
        self.pose.zero()
        zero = self.pose.snapshot()
        np.testing.assert_allclose(zero["quaternion"], (1, 0, 0, 0), atol=1e-8)
        self.assertEqual(zero["trail"], [])
        self.assertEqual(zero["elapsed_s"], 0)
        pose = self.sample(gyroscope=(0, 0, math.pi / 2), count=100)
        self.assertAlmostEqual(pose["euler_deg"]["yaw"], 45, delta=0.1)

    def test_yaw_is_visible_immediately_without_initial_calibration_wait(self):
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
        self.assertLess(np.linalg.norm(pose["position_m"]), .03)

    def test_short_vertical_translation_and_stationary_velocity_reset(self):
        self.sample(count=800)
        pose = self.sample(acceleration=(0, 0, GRAVITY + 1), count=200)
        self.assertGreater(pose["position_m"][2], .35)
        self.assertLess(pose["position_m"][2], .55)
        self.assertGreater(pose["velocity_m_s"][2], .85)
        self.assertFalse(pose["stationary"])
        pose = self.sample(count=200)
        self.assertTrue(pose["stationary"])
        np.testing.assert_allclose(pose["velocity_m_s"], 0, atol=1e-8)

    def test_zero_vectors_use_the_same_tilted_reference_as_orientation(self):
        gravity = (0, GRAVITY / math.sqrt(2), GRAVITY / math.sqrt(2))
        self.sample(acceleration=gravity, count=800)
        self.pose.zero()
        acceleration = (0, (GRAVITY + 1) / math.sqrt(2), (GRAVITY + 1) / math.sqrt(2))
        pose = self.sample(acceleration=acceleration, count=200)
        self.assertGreater(pose["position_m"][1], .2)
        self.assertAlmostEqual(pose["position_m"][1], pose["position_m"][2], delta=.01)
        np.testing.assert_allclose(pose["quaternion"], (1, 0, 0, 0), atol=1e-5)

    def test_sub_one_metre_acceleration_survives_low_pass_deadband(self):
        self.sample(count=200)
        pose = self.sample(acceleration=(0, 0, GRAVITY + .4), count=200)
        self.assertGreater(pose["position_m"][2], .13)
        self.assertGreater(pose["velocity_m_s"][2], .3)

    def test_pen_world_frame_stays_gravity_aligned_after_tilted_display_zero(self):
        gravity = (0, GRAVITY / math.sqrt(2), GRAVITY / math.sqrt(2))
        self.sample(acceleration=gravity, count=200)
        world_before = self.pose.world_motion()["quaternion"]
        self.pose.zero()
        np.testing.assert_allclose(self.pose.world_motion()["quaternion"], world_before)
        acceleration = (0, (GRAVITY + 1) / math.sqrt(2), (GRAVITY + 1) / math.sqrt(2))
        local = self.sample(acceleration=acceleration, count=200)
        world = self.pose.world_motion()
        self.assertGreater(world["acceleration_m_s2"][2], .9)
        self.assertAlmostEqual(world["acceleration_m_s2"][0], 0, delta=.001)
        self.assertAlmostEqual(world["acceleration_m_s2"][1], 0, delta=.001)
        self.assertGreater(local["linear_accel_m_s2"][1], .5)

    def test_world_residual_remains_available_for_idle_bias_estimation(self):
        local = self.sample(acceleration=(0, 0, GRAVITY + .1), count=800)
        self.assertTrue(local["stationary"])
        self.assertEqual(local["linear_accel_m_s2"], [0.0, 0.0, 0.0])
        world = self.pose.world_motion()
        self.assertAlmostEqual(world["acceleration_m_s2"][2], .1, delta=.001)
        self.pose.zero()
        np.testing.assert_allclose(self.pose.world_motion()["acceleration_m_s2"],
                                   world["acceleration_m_s2"])

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

    def test_gaps_do_not_integrate_unobserved_time(self):
        self.sample(count=800)
        before = self.sample(acceleration=(0, 0, GRAVITY + 1), count=100)
        self.tick += 10000
        pose = self.sample(acceleration=(0, 0, GRAVITY + 1))
        self.assertEqual(pose["status"], "gap")
        self.assertEqual(pose["position_m"], before["position_m"])
        self.assertEqual(pose["elapsed_s"], before["elapsed_s"])
        np.testing.assert_allclose(pose["velocity_m_s"], 0)
        pose = self.sample(count=100)
        self.assertEqual(pose["status"], "stationary")
        self.wall = 1
        pose = self.pose.snapshot()
        self.assertEqual(pose["status"], "gap")
        self.assertFalse(pose["stationary"])
        pose = self.sample()
        self.assertEqual(pose["status"], "gap")

    def test_invalid_samples_are_finite_and_do_not_move(self):
        self.sample(count=800)
        for field, value in (("ax", float("nan")), ("gy", float("inf")), ("az", 10000),
                             ("gx", -1000), ("t", -1), ("t", float("nan")), ("seq", float("inf"))):
            record = {"seq": self.seq + 1, "t": self.tick + 5, "ax": 0, "ay": 0,
                      "az": GRAVITY, "gx": 0, "gy": 0, "gz": 0}
            record[field] = value
            self.pose.feed(record)
            pose = self.pose.snapshot()
            self.assertEqual(pose["status"], "gap")
            np.testing.assert_allclose(pose["position_m"], 0)
            json.dumps(pose, allow_nan=False)

    def test_bounded_ten_hz_trail_and_snapshot_copy(self):
        pose = self.sample(count=199)
        self.assertLessEqual(len(pose["trail"]), 10)
        pose = self.sample(count=4000)
        self.assertEqual(len(pose["trail"]), 160)
        pose["trail"][0][0] = 123
        self.assertNotEqual(self.pose.snapshot()["trail"][0][0], 123)
        json.dumps(pose, allow_nan=False)

    def test_translation_limit_is_explicit_and_attitude_keeps_updating(self):
        self.sample(count=800)
        pose = self.sample(acceleration=(0, 0, GRAVITY + 100), count=100)
        self.assertTrue(pose["position_limited"])
        self.assertEqual(pose["status"], "gap")
        self.assertLess(np.linalg.norm(pose["velocity_m_s"]), MAX_SPEED_M_S)
        frozen = pose["position_m"]
        pose = self.sample(gyroscope=(0, 0, math.pi / 2), count=200)
        self.assertEqual(pose["position_m"], frozen)
        self.assertAlmostEqual(pose["euler_deg"]["yaw"], 90, delta=.1)
        self.pose.zero()
        self.assertFalse(self.pose.snapshot()["position_limited"])
        np.testing.assert_allclose(self.pose.snapshot()["position_m"], 0)

    def test_long_dynamic_input_is_bounded_and_strict_json_finite(self):
        self.sample(count=800)
        pose = self.sample(acceleration=(0, 0, GRAVITY + 1), count=1400)
        self.assertTrue(pose["position_limited"])
        self.assertLessEqual(np.linalg.norm(pose["position_m"]), MAX_DISPLACEMENT_M)
        self.assertGreater(np.linalg.norm(pose["position_m"]), 19)
        frozen = pose["position_m"]
        pose = self.sample(acceleration=(0, 0, GRAVITY + 1), count=10000)
        self.assertEqual(pose["position_m"], frozen)
        self.assertLessEqual(len(pose["trail"]), 160)
        json.dumps(pose, allow_nan=False)

    def test_reset_and_waiting_zero_do_not_reuse_a_previous_session(self):
        self.sample(count=800)
        self.sample(gyroscope=(0, 0, 1), count=100)
        self.pose.reset()
        self.pose.zero()
        pose = self.pose.snapshot()
        self.assertFalse(pose["available"])
        self.assertEqual(pose["status"], "waiting")
        self.assertIsNone(pose["timestamp_ms"])
        self.assertEqual(pose["trail"], [])
        self.tick = 0
        self.seq = 0
        pose = self.sample(count=100)
        self.assertTrue(pose["stationary"])
        np.testing.assert_allclose(pose["quaternion"], (1, 0, 0, 0), atol=1e-8)


if __name__ == "__main__":
    unittest.main()
