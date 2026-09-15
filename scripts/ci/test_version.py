import unittest
from release_version import bump, classify, parts


class VersionTests(unittest.TestCase):
    def test_user_version_rules(self):
        cases = [('refactor(vcu): restructure', [], '2.0.0'),
                 ('refact: cleanup', [], '2.0.0'),
                 ('feat: add console', ['release:major'], '2.0.0'),
                 ('feat: add button', [], '1.3.0'),
                 ('fix: correct path', [], '1.2.4'),
                 ('ci: add matrix', [], '1.2.4')]
        for title, labels, expected in cases:
            with self.subTest(title=title):
                self.assertEqual(bump('1.2.3', classify(title, labels)), expected)

    def test_highest_change_wins(self):
        self.assertEqual(bump('1.2.3', max(map(classify, ['fix: a', 'refactor: b', 'feat: c']))), '2.0.0')
        self.assertEqual(classify('refactor: b', ['release:patch']), 2)

    def test_breaking_marker(self):
        self.assertEqual(classify('feat(control)!: change protocol'), 2)

    def test_reject_bad_versions_and_labels(self):
        for value in ['1.2', 'v1.2.3', '01.2.3', '1.2.3-rc1', '1.2.3\n']:
            with self.assertRaises(ValueError):
                parts(value)
        with self.assertRaises(ValueError):
            classify('feat: test', ['release:big'])
