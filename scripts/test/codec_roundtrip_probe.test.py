import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from codec_roundtrip_probe import run

SETTINGS = dict(codec='h264',width=128,height=96,fps=20,bitrate_kbps=125,gop=30)


@unittest.skipUnless(os.name=='posix', 'codec worker is built only on Linux/macOS')
class WorkerTests(unittest.TestCase):
    def execute(self, body, encoders=('cpu',), timeout=1000):
        with tempfile.TemporaryDirectory() as root:
            worker=Path(root)/'worker'
            worker.write_text('#!'+sys.executable+'\nimport sys,json,time\n'+body)
            worker.chmod(0o700)
            return run(worker,encoders,['avdec_h264'],SETTINGS,timeout)

    @staticmethod
    def success():
        return """report=dict(schema='codec-roundtrip-v1',passed=True,backend=sys.argv[1],codec=sys.argv[2],decoder=sys.argv[3],
width=int(sys.argv[4]),height=int(sys.argv[5]),fps=int(sys.argv[6]),bitrate_kbps=int(sys.argv[7]),gop=int(sys.argv[8]),
initial_idr_valid=True,decoded_frames=1,encoded_running_pts_ns=0,decoded_pts_ns=0,
encoder_factory_available=True,decoder_factory_available=True)
"""

    def test_real_output_contract_and_ordered_fallback(self):
        result=self.execute(self.success()+"\nif sys.argv[1]=='nvenc': report.update(passed=False,error='device unavailable')\nprint(json.dumps(report))\n",('nvenc','cpu'))
        self.assertTrue(result['passed'])
        self.assertEqual(result['selected']['backend'],'cpu')
        self.assertEqual(len(result['attempts']),2)
        self.assertFalse(result['attempts'][0]['passed'])

    def test_timeout_including_teardown_does_not_pass(self):
        result=self.execute(self.success()+"print(json.dumps(report),flush=True)\ntime.sleep(30)\n",timeout=50)
        self.assertFalse(result['passed'])
        self.assertTrue(result['attempts'][0]['timeout'])
        self.assertLess(result['attempts'][0]['elapsed_ms'],2000)

    def test_invalid_or_missing_output(self):
        for body in ["print('not JSON')", "print('x'*70000)","print('{}')", "print('[]')"]:
            with self.subTest(body=body): self.assertFalse(self.execute(body)['passed'])

    def test_exit_code_identity_and_idr_evidence(self):
        for mutation in ["report['width']=1920", "report['initial_idr_valid']=False", "report['decoded_frames']=0",
                         "report['decoded_pts_ns']=1", "report['encoder_factory_available']=False"]:
            with self.subTest(mutation=mutation):
                self.assertFalse(self.execute(self.success()+mutation+"\nprint(json.dumps(report))")['passed'])
        self.assertFalse(self.execute(self.success()+"print(json.dumps(report))\nsys.exit(1)")['passed'])


if __name__=='__main__':unittest.main()
