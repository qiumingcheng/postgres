#!/usr/bin/env python3
"""Paired zero-margin metric analyzer; never a standalone product certificate."""
from __future__ import annotations
import argparse
import math
import random
from common import PACKAGE, read, write
from benchmark import quantile

def interval(ratios, alpha, reps, seed):
    if not ratios or any(not math.isfinite(v) or v<=0 for v in ratios):
        raise ValueError('ratios must be finite and positive')
    values=[math.log(v) for v in ratios]
    rng=random.Random(seed)
    n=len(values)
    samples=[sum(rng.choices(values,k=n))/n for _ in range(reps)]
    return math.exp(quantile(samples,alpha)),math.exp(quantile(samples,1-alpha))

def analyze(doc):
    spec=read(PACKAGE/'specs/acceptance.json')['product']
    # Input structure is kept distinct from the development pgbench report.
    if doc.get('schema')!='paired-independent-rounds-v1':
        raise ValueError('not a preregistered paired-round report')
    cases=doc['cases']
    if not cases or len({c['id'] for c in cases})!=len(cases):
        raise ValueError('empty or duplicate cases')
    family=doc['preregistered_metric_count']
    if family < 3*len(cases): raise ValueError('invalid multiplicity family')
    alpha=spec['familywise_alpha']/family
    reps=spec['bootstrap_replicates']
    if reps*alpha<100: raise ValueError('insufficient bootstrap tail resolution; preregister more replicates')
    output=[]
    for c in cases:
        rounds=c['rounds']
        if len({r['pair_id'] for r in rounds}) != len(rounds): raise ValueError('duplicate pair IDs')
        enough=len(rounds)>=spec['min_independent_rounds_for_analyzer']
        if any(min(r['baseline']['successes'],r['candidate']['successes'])<spec['p99_min_successful_transactions_per_round'] for r in rounds): enough=False
        metrics={}
        for index,key in enumerate(('throughput','p95_ms','p99_ms')):
            ratios=[r['candidate'][key]/r['baseline'][key] for r in rounds]
            lo,hi=interval(ratios,alpha,reps,spec['bootstrap_seed']+index)
            if not enough or all(v == 1.0 for v in ratios): status='INCONCLUSIVE'
            elif key=='throughput': status='PASS' if lo>=1 else ('FAIL' if hi<1 else 'INCONCLUSIVE')
            else: status='PASS' if hi<=1 else ('FAIL' if lo>1 else 'INCONCLUSIVE')
            metrics[key]={'lower':lo,'upper':hi,'status':status,'adjusted_one_sided_alpha':alpha}
        output.append({'id':c['id'],'metrics':metrics})
    statuses=[m['status'] for c in output for m in c['metrics'].values()]
    metric_status='FAIL' if 'FAIL' in statuses else ('INCONCLUSIVE' if 'INCONCLUSIVE' in statuses else 'PASS')
    return {'metric_status':metric_status,'cases':output,'product_gate':'NOT_EVALUATED',
            'method':'paired independent round log-ratio percentile bootstrap + Bonferroni',
            'required_external_evidence':['pre-registered complete workload family and sample plan',
                'same offered load for tail latency, capacity measured separately',
                'equal resources/constraints/persistence and natural plans',
                'correctness/security, maintenance debt and all product gates',
                'raw successful/failed/retried request records and build identities']}

if __name__=='__main__':
    a=argparse.ArgumentParser(description=__doc__)
    a.add_argument('input');a.add_argument('output')
    x=a.parse_args();result=analyze(read(x.input));write(x.output,result)
    print(result['metric_status']+'; product gate NOT_EVALUATED')
