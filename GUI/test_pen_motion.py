"""KEY boundaries and corrected physical strokes through the public adapter."""

import json
import unittest
from unittest import mock

import numpy as np

from pen_motion import PenMotion
from trajectory import GRAVITY


class PenMotionTests(unittest.TestCase):
    def setUp(self):
        self.motion = PenMotion()
        self.tick = 0

    def samples(self, linear=(0, 0, 0), key=False, stationary=False, count=1):
        for _ in range(count):
            self.tick = (self.tick + 5) & 0xFFFFFFFF
            position = self.motion.feed(self.tick, np.array(linear) + (0, 0, GRAVITY),
                                        (0, 0, 0), (1, 0, 0, 0), stationary, key)
        return position

    def stroke(self, axis=0, bias=(0, 0, 0)):
        self.samples()
        self.samples(key=True)
        acceleration = np.array(bias, dtype=float)
        acceleration[axis] += 1
        self.samples(acceleration, key=True, count=80)
        acceleration[axis] -= 2
        self.samples(acceleration, key=True, count=80)
        return self.samples()

    def test_key_up_motion_never_draws_and_release_freezes_corrected_endpoint(self):
        self.samples((4, 3, 2), count=1000)
        self.assertEqual(self.motion.snapshot()["position_m"], [0.0] * 3)
        endpoint = self.stroke()
        self.assertAlmostEqual(endpoint[0], .16, delta=.002)
        self.assertEqual(self.samples((2, -4, 1), count=1000), endpoint)
        self.assertEqual(self.motion.snapshot()["velocity_m_s"], [0.0] * 3)
        self.assertEqual(self.motion.snapshot()["algorithm"], "gaitmap-eskf-rts")

    def test_xyz_open_strokes_remain_three_dimensional_after_smoothing(self):
        for axis in range(3):
            self.motion.reset()
            endpoint = self.stroke(axis)
            self.assertAlmostEqual(endpoint[axis], .16, delta=.002)
            for other in range(3):
                if axis != other:
                    self.assertAlmostEqual(endpoint[other], 0, delta=1e-8)
            correction = self.motion.corrections()[0]
            np.testing.assert_allclose(correction["points"][0], 0, atol=1e-10)
            np.testing.assert_allclose(correction["points"][-1], endpoint)

    def test_release_corrects_biased_whole_stroke_instead_of_just_stopping(self):
        self.samples()
        self.samples(key=True)
        self.samples((1.12, 0, .1), key=True, count=80)
        preview = self.samples((-.88, 0, .1), key=True, count=80)
        final = self.samples()
        self.assertGreater(abs(preview[0] - .16), .02)
        self.assertLess(abs(final[0] - .16), .012)
        self.assertLess(abs(final[2]), .003)
        points = self.motion.corrections()[0]["points"]
        self.assertGreater(len(points), 30)
        self.assertNotEqual(final, preview)

    def test_second_stroke_starts_at_corrected_endpoint(self):
        first = self.stroke(0, (.1, 0, .1))
        second = self.stroke(2)
        np.testing.assert_allclose(self.motion.corrections()[1]["points"][0], first)
        self.assertAlmostEqual(second[2] - first[2], .16, delta=.002)
        self.assertEqual([c["stroke_id"] for c in self.motion.corrections()], [1, 2])

    def test_held_on_connect_requires_release_then_no_calibration_wait(self):
        self.samples((1, 0, 0), key=True, count=100)
        self.assertEqual(self.motion.reason, "release_required")
        self.assertEqual(self.motion.snapshot()["position_m"], [0.0] * 3)
        self.samples()
        self.samples(key=True)
        self.assertTrue(self.motion.recording)
        self.assertGreater(self.samples((1, 0, 0), key=True)[0], 0)

    def test_gap_and_invalid_data_preserve_ink_without_smoothing_partial_stroke(self):
        self.stroke()
        self.samples(key=True)
        before = self.samples((1, 0, 0), key=True, count=20)
        self.tick += 1000
        self.assertEqual(self.samples(key=True), before)
        self.assertEqual(self.motion.reason, "sample_gap")
        self.assertEqual(self.samples(key=True, count=20), before)
        self.assertEqual(len(self.motion.corrections()), 1)
        self.samples()
        self.samples(key=True)
        self.motion.invalidate()
        self.samples()
        self.assertEqual(len(self.motion.corrections()), 1)

    def test_invalid_key_or_sensor_data_cannot_start_stroke(self):
        for changes in ({"key_down": None}, {"key_down": 1}, {"stationary": 1},
                        {"acceleration": (float("nan"), 0, 0)}, {"gyroscope": (0, 0)},
                        {"quaternion": (0, 0, 0, 0)}, {"t_ms": -1}):
            self.motion.reset()
            self.samples()
            args = dict(t_ms=self.tick + 5, acceleration=(0, 0, GRAVITY),
                        gyroscope=(0, 0, 0), quaternion=(1, 0, 0, 0),
                        stationary=False, key_down=True)
            args.update(changes)
            self.motion.feed(**args)
            self.assertFalse(self.motion.recording)
            self.assertEqual(self.motion.reason, "invalid_sample")
            json.dumps(self.motion.snapshot(), allow_nan=False)

    def test_stationary_bias_is_constrained_without_velocity_leak_or_deadband(self):
        self.samples()
        self.samples(key=True)
        self.samples((.07, -.03, .08), stationary=True, key=True, count=1000)
        self.assertLess(np.linalg.norm(self.motion.snapshot()["position_m"]), .005)
        self.samples()
        self.assertLess(np.linalg.norm(self.motion.snapshot()["position_m"]), .005)

    def test_counter_wrap_does_not_split_stroke(self):
        self.tick = 0xFFFFFFF0
        endpoint = self.stroke()
        self.assertLess(self.tick, 1000)
        self.assertAlmostEqual(endpoint[0], .16, delta=.002)
        self.assertFalse(self.motion.blocked)

    def test_bounded_history_reset_and_detached_snapshots(self):
        for _ in range(10):
            self.stroke()
        self.assertEqual(len(self.motion.corrections()), 8)
        correction = self.motion.corrections()[0]
        correction["points"][0][0] = 999
        self.assertNotEqual(self.motion.corrections()[0]["points"][0][0], 999)
        old_id = self.motion.snapshot()["stroke_id"]
        self.motion.reset()
        self.assertEqual(self.motion.corrections(), [])
        self.assertEqual(self.motion.reason, "release_required")
        self.stroke()
        self.assertGreater(self.motion.snapshot()["stroke_id"], old_id)
        with mock.patch("pen_motion.MAX_STROKE_SAMPLES", 2):
            self.samples(key=True, count=4)
            self.assertTrue(self.motion.blocked)
            self.assertEqual(self.motion.reason, "stroke_capacity")


if __name__ == "__main__":
    unittest.main()
