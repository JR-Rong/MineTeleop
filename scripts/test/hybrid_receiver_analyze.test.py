import unittest
from hybrid_receiver_analyze import browser_transport_evidence, presentation_summary, select_window, require_network_pairs, path_summary


class HybridMeasurementTests(unittest.TestCase):
    @staticmethod
    def transport_sample(protocol):
        return {'stats': [dict(id='video', type='inbound-rtp', kind='video', transportId='transport'),
            dict(id='transport', type='transport', selectedCandidatePairId='pair'),
            dict(id='pair', type='candidate-pair', state='succeeded', localCandidateId='local', remoteCandidateId='remote'),
            dict(id='local', type='local-candidate', candidateType='host', protocol=protocol),
            dict(id='remote', type='remote-candidate', candidateType='host', protocol=protocol)]}

    def test_tcp_fallback_is_not_udp_evidence(self):
        self.assertFalse(browser_transport_evidence([self.transport_sample('tcp')])['direct_udp_only'])
        self.assertTrue(browser_transport_evidence([self.transport_sample('udp')])['direct_udp_only'])

    def test_final_udp_does_not_hide_earlier_tcp(self):
        result = browser_transport_evidence([self.transport_sample('tcp'), self.transport_sample('udp')])
        self.assertFalse(result['direct_udp_only'])
        self.assertEqual(len(result['selections']), 2)

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

    def test_wan_rejects_any_lan_tcp_or_relay_sample(self):
        network = dict(kind='wan-ipv4-udp', localIp='192.168.1.2', localPublicIp='8.8.8.8', remotePublicIp='1.1.1.1')
        pair = dict(local_address='192.168.1.2', remote_address='1.1.1.1', local_transport='udp', remote_transport='udp', local_type='host', remote_type='srflx')
        require_network_pairs([pair], 'udp', network)
        for change in [dict(remote_address='192.168.2.2'), dict(remote_type='relay'), dict(local_transport='tcp'), dict(local_address='')]:
            with self.assertRaises(AssertionError):
                require_network_pairs([dict(pair, **change), pair], 'udp', network)
        with self.assertRaises(AssertionError):
            require_network_pairs([], 'udp', network)

    def test_ipv6_and_explicit_relay_arm(self):
        network = dict(kind='wan-dualstack', family='ipv6', allowRelay=False,
                       localAddresses=['2001:4860::1'], localPublicAddresses=['2001:4860::1'],
                       remotePublicAddresses=['2606:4700::1'])
        pair = dict(local_address='2001:4860::1', remote_address='2606:4700::1',
                    local_transport='udp', remote_transport='udp', local_type='host', remote_type='host')
        require_network_pairs([pair], 'udp', network)
        with self.assertRaises(AssertionError):
            require_network_pairs([dict(pair, remote_address='fe80::1')], 'udp', network)
        relay = dict(pair, remote_address='2606:4700::2', remote_type='relay')
        with self.assertRaises(AssertionError):
            require_network_pairs([relay], 'udp', network)
        with self.assertRaisesRegex(AssertionError, 'TURN leg'):
            require_network_pairs([relay], 'udp', dict(network, allowRelay=True))
        with self.assertRaises(AssertionError):
            require_network_pairs([pair], 'udp', dict(network, family='ipv4'))

    def test_turn_leg_protocol_and_server_identity(self):
        url='turn:turn.example.test:3478?transport=udp'
        network=dict(kind='wan-dualstack',family='ipv4',allowRelay=True,localAddresses=['192.168.1.2'],localPublicAddresses=['8.8.8.8'],remotePublicAddresses=['1.1.1.1'],
                     turnServers=[dict(url=url,serverAddresses=['9.9.9.9'],relayAddresses=['9.9.9.10'])])
        pair=dict(local_address='9.9.9.10',remote_address='1.1.1.1',local_type='relay',remote_type='srflx',local_transport='udp',remote_transport='udp',local_relay_protocol='udp',local_url=url,signaled_turn_urls=[url])
        require_network_pairs([pair], 'udp', network)
        for change in [dict(local_relay_protocol='tcp'),dict(local_relay_protocol='tls'),dict(local_relay_protocol=None),dict(local_url=None),dict(signaled_turn_urls=[]),dict(local_address='8.8.4.4'),dict(remote_address='8.8.4.4'),dict(remote_type='relay')]:
            with self.subTest(change=change), self.assertRaises(AssertionError):
                require_network_pairs([dict(pair,**change)], 'udp', network)
        for protocol in ['udp','tcp','tls',None]:
            sample=self.transport_sample('udp')
            sample['stats'][-2].update(candidateType='relay',relayProtocol=protocol,url=url)
            sample['signaledTurnUrls']=[url]
            extracted=browser_transport_evidence([sample])['selections'][0]['pair']
            self.assertEqual(extracted['local_relay_protocol'],protocol)
            self.assertEqual(extracted['local_url'],url)

    def test_path_history_keeps_relay_and_excludes_sampling_gaps(self):
        direct = dict(local_address='2001:4860::1', remote_address='2606:4700::1', local_type='host', remote_type='host')
        relay = dict(direct, remote_type='relay')
        summary = path_summary([(0,relay),(1000,relay),(2000,direct),(3000,direct),(9000,direct)])
        self.assertEqual(summary['selected_hold_seconds'], dict(direct=1, relay=2))
        self.assertEqual(summary['path_changes_observed'], 1)
        self.assertEqual(summary['relay_to_direct_changes_observed'], 1)
        self.assertAlmostEqual(summary['relay_fraction'], 2/3)

    def test_full_observation_keeps_large_gap_and_quantile_tails(self):
        frames = [dict(presentationTime=i*33, receiveTime=i*33-12, presentedFrames=i) for i in range(1000)]
        frames.append(dict(presentationTime=frames[-1]['presentationTime']+300, receiveTime=frames[-1]['presentationTime'], presentedFrames=1000))
        result = presentation_summary(frames)
        self.assertEqual(result['observed_gaps_over_250ms'], 1)
        self.assertEqual(result['observed_presentation_gap']['max_ms'], 300)
        self.assertEqual(result['observed_receive_to_presentation']['p50_ms'], 12)
        self.assertEqual(result['observed_receive_to_presentation']['max_ms'], 300)


if __name__=='__main__': unittest.main()
