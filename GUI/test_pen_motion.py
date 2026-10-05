"""Deterministic world-acceleration tests; no board or wall-clock sleeps."""

import json
import math
import unittest

from pen_motion import MAX_SPEED_M_S, MAX_STROKE_DISPLACEMENT_M, PenMotion


class PenMotionTests(unittest.TestCase):
    def setUp(self):
        self.motion = PenMotion()
        self.tick = 0

    def samples(self, acceleration=(0.0, 0.0, 0.0), stationary=False, key=False, count=1):
        position = None
        for _ in range(count):
            self.tick = (self.tick + 5) & 0xFFFFFFFF
            position = self.motion.feed(self.tick, acceleration, stationary, key)
        return position

    def test_stationary_jitter_for_minutes_cannot_accumulate_position(self):
        self.samples(stationary=True)
        for held in (False, True):
            for index in range(24000):
                noise = (0.04 * math.sin(index * 0.13),
                         0.035 * math.sin(index * 0.17),
                         0.025 * math.cos(index * 0.11))
                self.samples(noise, stationary=True, key=held)
            self.assertEqual(self.motion.snapshot()["position_m"], [0.0] * 3)
            self.assertEqual(self.motion.snapshot()["velocity_m_s"], [0.0] * 3)
            self.assertFalse(self.motion.blocked)

    def test_noise_deadband_without_stationary_flag_does_not_move(self):
        self.samples()
        self.samples((0.03, -0.025, 0.04), key=True, count=24000)
        self.assertEqual(self.motion.snapshot()["position_m"], [0.0] * 3)
        self.assertFalse(self.motion.blocked)

    def test_small_constant_residual_and_idle_bias_do_not_accumulate(self):
        self.samples()
        self.samples((0.04, 0, 0), key=True, count=24000)
        self.assertEqual(self.motion.snapshot()["position_m"], [0.0] * 3)
        self.motion.reset()
        bias = (0.1, -0.09, 0.06)
        self.samples(bias, stationary=True, count=1200)
        self.samples(bias, stationary=False, key=True, count=24000)
        self.assertEqual(self.motion.snapshot()["position_m"], [0.0] * 3)
        self.assertFalse(self.motion.blocked)

    def test_idle_motion_is_not_learned_as_sensor_bias(self):
        self.samples((0.2, 0, 0), stationary=False, count=1200)
        point = self.samples((0.2, 0, 0), key=True, count=200)
        self.assertGreater(point[0], 0.07)
        self.assertLess(point[0], 0.1)

    def test_released_key_freezes_position_and_discards_idle_motion(self):
        self.samples((4, 3, 2), count=1000)
        self.assertEqual(self.motion.snapshot()["position_m"], [0.0] * 3)
        moving = self.samples((1, 0, 0), key=True, count=100)
        self.assertGreater(moving[0], 0.09)
        frozen = self.samples((3, -2, 1), key=False, count=1000)
        self.assertEqual(frozen, moving)
        self.assertEqual(self.motion.snapshot()["velocity_m_s"], [0.0] * 3)
        self.assertEqual(self.samples(key=True), frozen)
        self.assertEqual(self.samples(key=True, count=100), frozen)

    def test_translational_acceleration_preserves_world_axis(self):
        for axis in range(3):
            with self.subTest(axis=axis):
                self.motion.reset()
                self.samples()
                pulse = [0.0] * 3
                pulse[axis] = 1.0
                self.samples(pulse, key=True, count=80)
                pulse[axis] = -1.0
                point = self.samples(pulse, key=True, count=80)
                self.assertGreater(point[axis], 0.10)
                self.assertLess(point[axis], 0.17)
                for other in range(3):
                    if other != axis:
                        self.assertEqual(point[other], 0.0)
                self.samples(stationary=True, key=True)
                self.assertEqual(self.motion.snapshot()["velocity_m_s"], [0.0] * 3)

    def test_cross_axis_translation_is_not_projected_onto_a_canvas(self):
        self.samples()
        point = self.samples((1.0, -0.5, 0.25), key=True, count=100)
        self.assertGreater(point[0], 0.09)
        self.assertAlmostEqual(point[1], -point[0] / 2)
        self.assertAlmostEqual(point[2], point[0] / 4)

    def test_zero_world_acceleration_never_creates_virtual_orientation_motion(self):
        self.samples()
        self.samples(key=True, count=5000)
        self.assertEqual(self.motion.snapshot()["position_m"], [0.0] * 3)
        self.assertTrue(self.motion.recording)

    def test_first_held_sample_requires_release_then_starts_without_wait(self):
        self.samples((1, 0, 0), key=True, count=100)
        self.assertEqual(self.motion.snapshot()["position_m"], [0.0] * 3)
        self.assertEqual(self.motion.reason, "release_required")
        self.samples()
        self.samples((1, 0, 0), key=True)
        self.assertTrue(self.motion.recording)
        self.assertGreater(self.samples((1, 0, 0), key=True)[0], 0)

    def test_gap_freezes_and_held_key_cannot_resume_until_released(self):
        self.samples()
        before = self.samples((1, 0, 0), key=True, count=100)
        self.tick += 1000
        self.assertEqual(self.samples((1, 0, 0), key=True), before)
        self.assertEqual(self.motion.reason, "sample_gap")
        self.assertEqual(self.samples((1, 0, 0), key=True, count=100), before)
        self.assertTrue(self.motion.blocked)
        self.samples()
        self.assertGreater(self.samples((1, 0, 0), key=True, count=100)[0], before[0])

    def test_upstream_invalidation_preserves_ink_and_requires_release(self):
        self.samples()
        before = self.samples((1, 0, 0), key=True, count=100)
        self.assertEqual(self.motion.invalidate(), before)
        self.assertEqual(self.motion.reason, "sample_gap")
        self.assertEqual(self.motion.snapshot()["velocity_m_s"], [0.0] * 3)
        self.assertEqual(self.samples((1, 0, 0), key=True, count=100), before)
        self.samples()
        self.assertGreater(self.samples((1, 0, 0), key=True, count=100)[0], before[0])

    def test_uint32_clock_wrap_is_continuous(self):
        self.tick = 0xFFFFFFE0
        self.samples()
        point = self.samples((1, 0, 0), key=True, count=100)
        self.assertLess(self.tick, 1000)
        self.assertGreater(point[0], 0.09)
        self.assertFalse(self.motion.blocked)

    def test_invalid_samples_and_missing_key_require_release(self):
        malformed = (
            (10, (float("nan"), 0, 0), False, True),
            (10, (0, float("inf"), 0), False, True),
            (10, (0, 0), False, True),
            (10, (1000, 0, 0), False, True),
            (10, (0, 0, 0), False, None),
            (10, (0, 0, 0), False, 1),
            (float("nan"), (0, 0, 0), False, True),
            (-1, (0, 0, 0), False, True),
        )
        for sample in malformed:
            with self.subTest(sample=sample):
                self.motion.reset()
                self.samples()
                before = self.samples((1, 0, 0), key=True, count=50)
                self.assertEqual(self.motion.feed(*sample), before)
                self.assertEqual(self.motion.reason, "invalid_sample")
                self.assertEqual(self.samples((1, 0, 0), key=True, count=50), before)
                json.dumps(self.motion.snapshot(), allow_nan=False)

    def test_large_bias_limits_are_explicit_and_freeze_until_release(self):
        for acceleration, reason in (((20, 0, 0), "speed_limit"), ((1, 0, 0), "stroke_limit")):
            with self.subTest(reason=reason):
                self.motion.reset()
                self.samples()
                frozen = self.samples(acceleration, key=True, count=4000)
                snapshot = self.motion.snapshot()
                self.assertTrue(snapshot["blocked"])
                self.assertEqual(snapshot["reason"], reason)
                self.assertLessEqual(math.hypot(*snapshot["velocity_m_s"]), MAX_SPEED_M_S)
                self.assertLessEqual(math.hypot(*frozen), MAX_STROKE_DISPLACEMENT_M)
                self.assertEqual(self.samples(acceleration, key=True, count=100), frozen)
                self.samples()
                self.assertFalse(self.motion.blocked)

    def test_reset_and_return_values_cannot_modify_internal_position(self):
        self.samples()
        point = self.samples((1, 0, 0), key=True, count=100)
        point[0] = 999
        snapshot = self.motion.snapshot()
        self.assertNotEqual(snapshot["position_m"][0], 999)
        snapshot["position_m"][0] = 999
        self.assertNotEqual(self.motion.snapshot()["position_m"][0], 999)
        self.motion.reset()
        self.assertEqual(self.motion.snapshot()["position_m"], [0.0] * 3)
        self.assertEqual(self.motion.reason, "release_required")


if __name__ == "__main__":
    unittest.main()
