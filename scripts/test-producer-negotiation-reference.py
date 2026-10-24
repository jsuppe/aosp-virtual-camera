#!/usr/bin/env python3
"""Opt-in A13 Cuttlefish producer-capability/FPS/lifecycle acceptance; no installs/policy changes."""
import argparse
import importlib.util
import json
import sys
import time
from pathlib import Path

spec=importlib.util.spec_from_file_location('vcam_e2e',Path(__file__).with_name('test-e2e-reference.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class Negotiation(module.Harness):
    def profile(self,number,name,reject=False):
        self.shell('am','start-foreground-service','-n',module.PRODUCERS[number-1]+'/com.example.vcamproducer.VCamProducerService',
            '--ez','e2e','true','--ei','producer',str(number),'--ei','run',str(self.run),'--es','profile',name,'--ez','reject_ready',str(reject).lower())
    def reset(self,width=640,height=480,fps=30,probe=False):
        self.stop_apps();self.run+=1
        self.shell('pm','grant',module.CONSUMER,'android.permission.CAMERA')
        self.shell('input','keyevent','KEYCODE_WAKEUP');self.shell('wm','dismiss-keyguard')
        return self.consumer(run100=self.run,run101=self.run,width100=width,height100=height,fps100=fps,
            width101=640,height101=480,fps101=30,probe_only=probe)
    def metadata(self,state,n=1):return self.cam(state,n).get('advertised_yuv',[])
    def expect_profile(self,width,height,maxfps,jpeg=True):
        def matches(s):
            c=self.cam(s,1);m=c.get('advertised_yuv',[])
            return len(m)==1 and (m[0]['width'],m[0]['height'])==(width,height) and \
                m[0]['min_duration_ns']==(1_000_000_000+maxfps-1)//maxfps and \
                max((r[1] for r in c.get('advertised_fps',[])),default=0)==maxfps and c.get('jpeg')==jpeg
        return self.wait(matches,'Camera2 descriptor-derived profile')
    def absence(self):return self.wait(lambda s:'100' not in s['inventory'],'real camera removal')
    def execute(self):
        self.current_phase='NEG01_descriptor_profiles_and_same_process_cache_refresh'
        self.reset(probe=True)
        consumer_pid=self.shell('pidof',module.CONSUMER)
        for name,w,h,f,jpeg in [('1080p30',1920,1080,30,True),('1080p60',1920,1080,60,True),
                ('4k30',3840,2160,30,True),('4k60',3840,2160,60,True),('1080p30-nojpeg',1920,1080,30,False),
                ('legacy-1080p30',1920,1080,30,True)]:
            self.profile(1,name);state=self.expect_profile(w,h,f,jpeg)
            (self.out/('profile-'+name+'.json')).write_text(json.dumps(state,indent=2))
            assert self.shell('pidof',module.CONSUMER)==consumer_pid,'probe process changed; cache refresh test invalid'
            self.shell('am','force-stop',module.PRODUCERS[0]);self.absence()
        self.passed('NEG01_descriptor_profiles_and_same_process_cache_refresh',state)

        self.current_phase='NEG02_mixed_resolution_specific_timing'
        self.profile(1,'mixed')
        state=self.wait(lambda s:len(self.metadata(s))==2,'mixed mode publication')
        modes={(m['width'],m['height']):m['min_duration_ns'] for m in self.metadata(state)}
        assert modes=={(1920,1080):16666667,(3840,2160):33333334},modes
        self.passed(self.current_phase,state)
        self.shell('am','force-stop',module.PRODUCERS[0]);self.absence()

        self.current_phase='NEG03_malformed_descriptor_not_published'
        self.profile(1,'invalid');time.sleep(3)
        state=self.state();assert '100' not in state['inventory']
        logs=self.shell('logcat','-d','-s','VCamProducer:I')
        assert 'invalid producer mode' in logs,'malformed registration rejection not observed'
        self.passed(self.current_phase,state)

        self.current_phase='NEG04_real_30_60_30_rate_changes_and_freshness'
        self.reset(1920,1080,30);self.profile(1,'1080p60')
        before=self.wait(lambda s:self.cam(s,1)['images']>=30,'1080p initial distinct frames')
        for fps,duration in [(60,16666667),(30,33333334)]:
            generation=self.cam(before,1)['generation']
            self.consumer(fps100=fps)
            state=self.wait(lambda s:self.cam(s,1)['generation']>generation and self.cam(s,1)['frame_duration_ns']==duration,
                'fresh new-rate configuration',timeout=25)
            state=self.advance(state,(1,),30);self.unchanged_lifecycle(before,state,1);before=state
        self.passed(self.current_phase,state)

        self.current_phase='NEG05_unsupported_rate_never_successful'
        self.reset(1920,1080,60);self.profile(1,'1080p30')
        state=self.wait(lambda s:s['failure'] or self.cam(s,1)['device_errors']>0 or self.cam(s,1)['capture_failures']>0,
            'explicit rejection of locked60 on30-only profile',allow_failure=True)
        assert self.cam(state,1)['images']==0 and self.cam(state,1)['completed']==0
        self.passed(self.current_phase,state)

        self.current_phase='NEG06_size_rate_cross_product_rejected'
        self.reset(3840,2160,60);self.profile(1,'mixed')
        state=self.wait(lambda s:s['failure'] or self.cam(s,1)['device_errors']>0 or self.cam(s,1)['capture_failures']>0,
            '4K60 rejected despite1080p60 in the same camera',allow_failure=True)
        assert self.cam(state,1)['images']==0 and self.cam(state,1)['completed']==0
        self.passed(self.current_phase,state)

        self.current_phase='NEG07_producer_rejection_no_target_success'
        self.reset();self.profile(1,'default',reject=True)
        before=self.wait(lambda s:self.cam(s,1)['capture_failures']>=3,'rejected source failed requests')
        state=self.no_output(1,before)
        assert self.cam(state,1)['images']==0 and self.cam(state,1)['completed']==0
        self.passed(self.current_phase,state)

        self.current_phase='NEG08_live_capability_replacement_and_other_camera_isolation'
        self.reset(1920,1080,30);self.profile(1,'1080p30');self.profile(2,'default')
        before=self.wait(lambda s:all(self.cam(s,n)['images']>=30 for n in (1,2)),'two heterogeneous producer streams')
        self.consumer(probe_only=True)
        self.shell('am','force-stop',module.PRODUCERS[0])
        absent=self.wait(lambda s:'100' not in s['inventory'] and self.cam(s,1)['disconnects']>self.cam(before,1)['disconnects'],
            'genuine removal/disconnection of old-profile consumer')
        self.unchanged_lifecycle(before,absent,2)
        self.run+=1;self.consumer(run100=self.run)
        self.profile(1,'4k60');state=self.expect_profile(3840,2160,60)
        self.consumer(probe_only=False,width100=3840,height100=2160,fps100=30)
        state=self.wait(lambda s:self.cam(s,1)['images']>=self.cam(before,1)['images']+30 and self.cam(s,2)['images']>=self.cam(before,2)['images']+30,
            'fresh replacement4K stream and uninterrupted second camera',timeout=25)
        self.unchanged_lifecycle(before,state,2)
        self.passed(self.current_phase,state)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--adb',default='adb');p.add_argument('--serial',required=True);p.add_argument('--output',required=True)
    p.add_argument('--allow-reference-mutations',action='store_true')
    args=p.parse_args();args.scenario='producer-negotiation';args.allow_hal_restart=False
    h=Negotiation(args);error=None;authorized=False
    try:h.preflight();authorized=True;h.execute()
    except Exception as e:error=str(e);print('FAIL:',e,flush=True)
    finally:
        if authorized:
            (h.out/'runtime.log').write_text(h.shell('logcat','-d','-s','VCamProducer:I','VCamE2E:I','VirtualCameraService:I','VCamRelayJni:I','VirtualCameraSession:I',check=False))
            h.stop_apps()
        (h.out/'results.json').write_text(json.dumps({'scope':'producer mode/FPS/readiness/republication reference acceptance','tests':h.results,
            'error':error,'unfinished_phase':h.current_phase if error else None,'release_ready':False},indent=2))
        h.command_log.close()
    return 1 if error else 0

if __name__=='__main__':sys.exit(main())
