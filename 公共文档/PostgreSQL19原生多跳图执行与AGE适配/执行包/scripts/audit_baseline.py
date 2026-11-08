#!/usr/bin/env python3
"""Record observed baseline failures without accepting them as product behavior."""
import re
from common import PACKAGE, WORKSPACE, capture, now, sha, write

def main():
    runs=sorted((PACKAGE/'evidence/runs').glob('*-regress-baseline-age'))
    runs=[p for p in runs if (p/'regression.diffs').exists() and '--dbname=contrib_regression' in (p/'regression.log').read_text()]
    if not runs: raise RuntimeError('no complete AGE regression with the correct database name')
    root=runs[-1]
    log=(root/'regression.log').read_text()
    failed=re.findall(r'^not ok\s+\d+\s+-\s+(\S+)',log,re.M)
    expected={'expr','cypher_match','cypher_call','jsonb_operators','subgraph','age_upgrade'}
    if set(failed)!=expected: raise RuntimeError('failure signature changed; classify the new results before updating the ledger')
    details={
        'expr':'函数错误参数数目的 HINT/DETAIL 消息与 PG19 当前输出不同。需审查后更新版本适配预期。',
        'cypher_match':'有无 ORDER BY 的语义需逐条确认；观察到四行路径的物理顺序差异，另有错误消息差异。',
        'cypher_call':'不存在函数或参数个数不匹配时，HINT 消失或改为 DETAIL。',
        'jsonb_operators':'不存在/参数类型不适用算子的 DETAIL/HINT 消息差异。',
        'subgraph':'嵌套动态 SQL 中不存在算子的错误消息差异。',
        'age_upgrade':'合成升级脚本报 load_labels_from_file 同参数签名已存在，upgraded_to_current 为 f；不能当作纯排版差异。'}
    record={'version':1,'recorded_at':now(),'status':'OPEN_BASELINE_DIFFERENCES','passed':37,'total':43,
            'failed_tests':[{'test':t,'observation':details[t],'resolution_task':'G0-035','status':'OPEN'} for t in failed],
            'evidence_files':[{'path':str((root/f).relative_to(PACKAGE)),'sha256':sha(root/f)} for f in ('regression.log','regression.out','regression.diffs')],
            'not_waived':True,'candidate_code_changes_included':False,
            'age_history':{'is_shallow':capture(['git','rev-parse','--is-shallow-repository'],WORKSPACE/'apache-age-pg19-port').strip()=='true',
                'reachable_commits':int(capture(['git','rev-list','--count','HEAD'],WORKSPACE/'apache-age-pg19-port')),
                'upgrade_inference':'本地仅 1 个提交，Makefile 以 git log -- age.control 选初始版本会选到当前 HEAD。合成升级起点需先核实；当前失败不能直接归为真实历史升级缺陷，也不能改成 PASS。'},
            'harness_fix':'Explicit --dbname=contrib_regression; the earlier 10 failures included harness-induced contamination and are retained as superseded diagnostic runs.'}
    write(PACKAGE/'baseline/known_issues.json',record)
    print('Recorded 6 unresolved AGE baseline differences; no tests marked PASS.')

if __name__=='__main__': main()
