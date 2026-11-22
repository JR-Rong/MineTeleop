import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location('analyze', pathlib.Path(__file__).with_name('receiver_ab_analyze.py'))
analyze = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyze)


class MeasurementTests(unittest.TestCase):
    def test_missed_callbacks_are_not_dropped_frames(self):
        # Forty seconds, 1200 decoded/presented frames, only every other
        # presentation observed by JS. Decode gaps alternate 28.333/38.333 ms.
        initial = dict(id='v', type='inbound-rtp', kind='video', framesDecoded=0,
            jitterBufferEmittedCount=0, totalInterFrameDelay=0, totalSquaredInterFrameDelay=0,
            totalDecodeTime=0, jitterBufferDelay=0, jitterBufferTargetDelay=0,
            framesDropped=0, packetsLost=0, freezeCount=0)
        final = dict(initial, framesDecoded=1200, jitterBufferEmittedCount=1200,
            totalInterFrameDelay=40, totalSquaredInterFrameDelay=1200*((1/30)**2+.005**2),
            totalDecodeTime=1.2, jitterBufferDelay=6, jitterBufferTargetDelay=18)
        data = dict(start=0, errors=[], visibility=[], longTasks=[], samples=[
            dict(at=10000, stats=[initial]), dict(at=50000, stats=[final], metrics={'selected_candidate_pair':None})],
            tracks=[{'frames':[dict(now=10000+i*200/3, presentationTime=10000+i*200/3, presentedFrames=i*2) for i in range(601)]}])
        result=analyze.browser(data)
        self.assertAlmostEqual(result['tracks'][0]['decoded_fps'],30)
        self.assertAlmostEqual(result['tracks'][0]['decode_gap_stddev_ms'],5)
        self.assertAlmostEqual(result['display'][0]['presented_counter_fps'],30)
        self.assertAlmostEqual(result['display'][0]['callback_coverage'],.5)

    def test_incomplete_window_rejected(self):
        with self.assertRaisesRegex(AssertionError,'window'):
            analyze.endpoints([{'at':49000}],0,'at')

    def test_interrupted_run_rejected(self):
        with self.assertRaisesRegex(AssertionError,'interrupted'):
            analyze.browser({'errors':['disconnected']})


if __name__ == '__main__':
    unittest.main()
