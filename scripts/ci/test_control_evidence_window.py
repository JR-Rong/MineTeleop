import importlib.util
from pathlib import Path
import unittest


source = Path(__file__).resolve().parents[1] / 'test' / 'control_evidence_window.py'
spec = importlib.util.spec_from_file_location('control_evidence_window', source)
window = importlib.util.module_from_spec(spec)
spec.loader.exec_module(window)


class ControlEvidenceWindowTests(unittest.TestCase):
    def test_closed_archive_tail_survives_a_slower_observer_host_clock(self):
        commands = [{'stage': 'send_completed', 'seq': seq, 'command_sent_at_utc_ms': stamp}
                    for seq, stamp in enumerate((100000, 101500, 102100, 102700), 1)]
        # The observer ended at101000 on a different clock. It cannot truncate
        # real product-clock sends in an already retired archive.
        cutoff = window.product_window_cutoff_ms(100000, 600)
        issues = []
        selected = window.dispatches_before_product_cutoff(commands, cutoff, issues)
        self.assertEqual([command['seq'] for command in selected], [1, 2, 3, 4])
        self.assertEqual(issues, [])

    def test_planned_duration_excludes_later_cleanup_and_keeps_exact_boundary(self):
        cutoff = window.product_window_cutoff_ms(100000, 600)
        commands = [{'stage': stage, 'seq': seq, 'command_sent_at_utc_ms': stamp}
                    for seq, (stage, stamp) in enumerate((('send_completed', 699999),
                        ('send_failed', 700000), ('send_skipped', 700001)), 1)]
        self.assertEqual([command['seq'] for command in
            window.dispatches_before_product_cutoff(commands, cutoff, [])], [1, 2])

    def test_missing_or_invalid_dispatch_time_is_explicit(self):
        issues = []
        commands = [{'stage': 'send_completed', 'seq': seq, 'command_sent_at_utc_ms': stamp}
                    for seq, stamp in enumerate((None, True, '100000', float('nan'), float('inf'), -1), 1)]
        self.assertEqual(window.dispatches_before_product_cutoff(commands, 700000, issues), [])
        self.assertEqual([issue['seq'] for issue in issues], [1, 2, 3, 4, 5, 6])
        self.assertTrue(all(issue['kind'] == 'dispatch_timestamp_unavailable' for issue in issues))

    def test_application_and_rejection_evidence_is_preserved_without_filling(self):
        commands = [{'stage': 'receive_apply', 'seq': 1, 'accepted': False, 'receive_apply_invoked': False},
                    {'stage': 'vehicle_transport_result_received', 'seq': 1, 'accepted': False}]
        selected = window.dispatches_before_product_cutoff(iter(commands), 700000, [])
        self.assertEqual(selected, commands)
        self.assertIs(selected[0], commands[0])
        self.assertFalse(commands[0]['receive_apply_invoked'])

    def test_invalid_window_is_rejected(self):
        for start, duration in ((True, 600), (-1, 600), (None, 600), (0, 0),
                                (0, -1), (0, False), (0, float('inf'))):
            with self.subTest(start=start, duration=duration), self.assertRaises(ValueError):
                window.product_window_cutoff_ms(start, duration)
        for cutoff in (None, True, -1, float('nan'), float('inf')):
            with self.subTest(cutoff=cutoff), self.assertRaises(ValueError):
                window.dispatches_before_product_cutoff([], cutoff, [])


if __name__ == '__main__':
    unittest.main()
