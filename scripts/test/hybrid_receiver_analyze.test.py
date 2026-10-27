import unittest
from hybrid_receiver_analyze import presentation_summary, select_window


class HybridMeasurementTests(unittest.TestCase):
    def test_ui_callbacks_do_not_define_display_fps(self):
        frames=[dict(presentationTime=i*100, receiveTime=i*100-12, presentedFrames=i*3) for i in range(11)]
        result=presentation_summary(frames)
        self.assertAlmostEqual(result['presented_fps'],30)
        self.assertAlmostEqual(result['callback_coverage'],1/3)
        self.assertEqual(result['observed_receive_to_presentation']['p99_ms'],12)

    def test_missing_stats_window_is_rejected(self):
        with self.assertRaisesRegex(AssertionError,'coverage'):
            select_window([{'at':0},{'at':10000}],0,4000,9000,'at')

    def test_reset_presentation_counter_is_rejected(self):
        with self.assertRaisesRegex(AssertionError,'counter'):
            presentation_summary([dict(presentationTime=1,presentedFrames=10),dict(presentationTime=2,presentedFrames=1)])


if __name__=='__main__': unittest.main()
