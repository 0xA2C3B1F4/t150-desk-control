import copy
import io
import json
from pathlib import Path
import sys
import unittest
import zipfile

import yaml

import test_release as fixtures
ROOT = fixtures.ROOT
sys.path.insert(0,str(ROOT/"tools"))
import release
import release_github as gh


class FakeAPI:
    def __init__(self, root, commit):
        self.commit=commit
        self.run={"id":7,"repository":{"full_name":release.REPOSITORY},"workflow_id":9,
                  "event":"workflow_dispatch","head_branch":"main","head_sha":commit,
                  "status":"completed","conclusion":"success"}
        self.jobs=[{"name":"tests / "+name,"conclusion":"success"} for name in gh.required_jobs(root)]
        self.releases=[];self.assets=[];self.writes=[];self.zip_bytes=b''

    def request(self,path,method='GET',data=None,raw=False,upload=False):
        if method!='GET':
            self.writes.append((path,method,data))
            if upload:return {"digest":"sha256:"+release.digest(data)}
            if method=='DELETE':return None
            assert data['draft'] is True
            return dict(data,id=11,html_url='https://example.invalid/draft')
        if '/actions/workflows/' in path:return {"id":9}
        if '/actions/runs/' in path:return self.run
        if '/git/ref/tags/' in path:return {"object":{"type":"tag","sha":"a"*40}}
        if '/git/tags/' in path:return {"object":{"type":"commit","sha":self.commit}}
        raise AssertionError(path)

    def pages(self,path,key=None):
        if '/jobs?' in path:return self.jobs
        if '/artifacts' in path:return [{"id":17,"name":"release-package","expired":False}]
        if path.endswith('/assets'):return self.assets
        if path.endswith('/releases'):return self.releases
        raise AssertionError(path)

    def archive(self,artifact_id):return self.zip_bytes


class GitHubReleaseTest(unittest.TestCase):
    setUp=fixtures.ReleaseTest.setUp
    git=fixtures.ReleaseTest.git
    package=fixtures.ReleaseTest.package

    def report(self,api,stage='candidate-check'):
        return gh.build_report(api,self.root,self.version,self.sha,self.sha,7,stage,7 if stage=="tagged-draft" else None)

    def test_required_jobs_match_workflow_matrices(self):
        jobs = yaml.safe_load((ROOT/'.github/workflows/test.yml').read_text())['jobs']
        expected = set()
        for job_id in ('native', 'esphome', 'github-example'):
            self.assertNotIn('name', jobs[job_id])
            matrix = jobs[job_id]['strategy']['matrix']
            self.assertEqual(len(matrix), 1)
            for value in next(iter(matrix.values())):
                expected.add(f'{job_id} ({value})')
        for job_id in ('release-checks', 'release-examples'):
            self.assertNotIn('name', jobs[job_id])
            expected.add(job_id)
        self.assertEqual(gh.required_jobs(ROOT), expected)

    def test_missing_failed_skipped_jobs_and_foreign_run_rejected(self):
        api=FakeAPI(self.root,self.sha)
        gh.validate_report(self.report(api),self.version,self.sha)
        for state in ['skipped','failure','cancelled',None]:
            api.jobs[0]['conclusion']=state
            with self.assertRaises(ValueError):self.report(api)
        api.jobs[0]['conclusion']='success';api.jobs.pop()
        with self.assertRaises(ValueError):self.report(api)
        for update in [dict(repository={'full_name':'other/repo'}),dict(workflow_id=10),
                       dict(event='pull_request'),dict(head_branch='feature'),dict(conclusion='failure')]:
            run=copy.deepcopy(api.run);run.update(update)
            with self.assertRaises(ValueError):gh.verify_run(run,9,True)

    def test_candidate_artifact_bound_to_actual_run(self):
        api=FakeAPI(self.root,self.sha)
        report=self.report(api)
        package=release.package_bytes(self.root,self.version,self.sha)
        data=io.BytesIO()
        with zipfile.ZipFile(data,'w') as archive:
            for name,content in package.items():archive.writestr('package/'+name,content)
            archive.writestr('validation-report.json',release.json_bytes(report))
        api.zip_bytes=data.getvalue()
        gh.previous_candidate(api,7,Path(self.temp.name)/'previous',self.root,self.version,self.sha)
        api.run['conclusion']='failure'
        with self.assertRaises(ValueError):gh.previous_candidate(api,7,Path(self.temp.name)/'bad',self.root,self.version,self.sha)

    def test_zip_allowlist(self):
        data=io.BytesIO()
        with zipfile.ZipFile(data,'w') as archive:archive.writestr('../escape','invalid')
        with self.assertRaises(ValueError):gh.unpack(data.getvalue(),Path(self.temp.name)/'bad-zip')

    def test_draft_create_update_and_published_rejection(self):
        api=FakeAPI(self.root,self.sha)
        self.git('tag','-a','v'+self.version,'-m','fixture')
        directory=Path(self.temp.name)/'output';directory.mkdir()
        package=self.package();package.rename(directory/'package')
        (directory/'validation-report.json').write_bytes(release.json_bytes(self.report(api,'tagged-draft')))
        gh.draft(api,directory,self.root,self.version,self.sha,7,self.sha)
        self.assertTrue(api.writes)
        api.writes=[];api.releases=[{'id':11,'tag_name':'v'+self.version,'target_commitish':self.sha,'draft':True}]
        gh.draft(api,directory,self.root,self.version,self.sha,7,self.sha)
        self.assertEqual(api.writes[0][1],'PATCH')
        api.writes=[];api.releases[0]['draft']=False
        with self.assertRaises(ValueError):gh.draft(api,directory,self.root,self.version,self.sha,7,self.sha)
        self.assertEqual(api.writes,[])
        api.releases[0]['draft']=True;api.releases[0]['target_commitish']='b'*40
        with self.assertRaises(ValueError):gh.draft(api,directory,self.root,self.version,self.sha,7,self.sha)
        self.assertEqual(api.writes,[])


if __name__=='__main__':unittest.main()
