import unittest
import json
import tempfile
from pathlib import Path
from hybrid_receiver_analyze import browser_transport_evidence, presentation_summary, select_window, require_network_pairs, path_summary, fallback_validation, analyze


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

    def fallback_fixture(self):
        url='turn:turn.example.test:3478?transport=udp'
        network=dict(kind='wan-dualstack',family='ipv4',allowRelay=True,validationGoal='fallback',localAddresses=['192.168.1.2'],localPublicAddresses=['8.8.8.8'],remotePublicAddresses=['1.1.1.1'],
                     turnServers=[dict(url=url,serverAddresses=['9.9.9.9'],relayAddresses=['9.9.9.10'])])
        scope=dict(sourceSessionId='s',sourceControlSessionGeneration=2)
        samples=[]
        for at, count in [(200,10),(1200,20)]:
            sample=self.transport_sample('udp')
            sample.update(at=at,negotiationId='retry',signaledTurnUrls=[url],**scope)
            sample['stats'][0].update(bytesReceived=count*100,framesDecoded=count)
            sample['stats'][2].update(bytesReceived=count*200)
            sample['stats'][3].update(address='9.9.9.10',candidateType='relay',relayProtocol='udp',url=url)
            sample['stats'][4].update(address='1.1.1.1')
            samples.append(sample)
        return dict(mode='browser',start=200,end=1300,samples=samples,attempts=[dict(at=0,id='direct',mode='direct',**scope),dict(at=100,id='retry',mode='all',**scope)]),network

    def test_fallback_requires_transition_relay_and_media_progress(self):
        data,network=self.fallback_fixture()
        self.assertTrue(fallback_validation(data,network)['verified'])
        # Permission alone remains valid for an ordinary media experiment.
        self.assertFalse(fallback_validation(data,dict(network,validationGoal='media'))['verified'])
        for sample in data['samples']:
            sample['stats'][3].update(address='192.168.1.2',candidateType='host')
        with self.assertRaisesRegex(AssertionError,'relay observations'):
            fallback_validation(data,network)
        self.assertFalse(fallback_validation(data,dict(network,validationGoal='media'))['required'])

    def test_fallback_cannot_use_stale_scope_unknown_turn_or_stalled_media(self):
        for defect in ['same_id','no_direct','wrong_session','old_sample','tcp','no_frames','no_inbound_bytes','no_pair_bytes','counter_reset','long_gap','changed_pair','native_unknown_progress']:
            data,network=self.fallback_fixture()
            if defect=='same_id':data['attempts'][0]['id']='retry'
            if defect=='no_direct':data['attempts'][0]['mode']='all'
            if defect=='wrong_session':data['attempts'][0]['sourceSessionId']='old'
            if defect=='old_sample':data['samples'][1]['negotiationId']='direct'
            if defect=='tcp':data['samples'][0]['stats'][3]['relayProtocol']='tcp'
            if defect=='no_frames':data['samples'][1]['stats'][0]['framesDecoded']=10
            if defect=='no_inbound_bytes':data['samples'][1]['stats'][0]['bytesReceived']=1000
            if defect=='no_pair_bytes':data['samples'][1]['stats'][2]['bytesReceived']=2000
            if defect=='counter_reset':data['samples'][1]['stats'][0]['bytesReceived']=0
            if defect=='long_gap':data['samples'][1]['at']=5000
            if defect=='changed_pair':data['samples'][1]['stats'][2]['remoteCandidateId']='other'
            if defect=='native_unknown_progress':data['mode']='native'
            with self.subTest(defect=defect),self.assertRaises(AssertionError):fallback_validation(data,network)

    def test_actual_analyzer_rejects_all_direct_fallback_run(self):
        data,network=self.fallback_fixture()
        data.update(expectedNetwork=network,expectedTransport='udp',seconds=1,errors=[],visibility=[])
        for sample in data['samples']:
            sample['stats'][3].update(address='192.168.1.2',candidateType='host')
        with tempfile.TemporaryDirectory() as root:
            Path(root,'ui.json').write_text(json.dumps(data))
            Path(root,'metadata.json').write_text(json.dumps(dict(errors=[],expected_network=network)))
            with self.assertRaisesRegex(AssertionError,'relay observations'):
                analyze(root)

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
