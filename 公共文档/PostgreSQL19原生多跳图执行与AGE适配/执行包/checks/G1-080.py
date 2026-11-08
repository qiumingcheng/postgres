#!/usr/bin/env python3
"""Fail closed until measured calibration and holdout evidence are implemented."""
import json

if __name__ == '__main__':
    print(json.dumps({
        'task_id': 'G1-080', 'status': 'NOT_IMPLEMENTED',
        'assertions_passed': [],
        'reason': 'Prior acceptance withdrawn: synthetic coefficients and a '
                  'literal qerror=0 cannot establish natural CBO performance.',
        'required': ['measured frontend/execution times', 'independent holdout',
                     'measured q-error/zero-result error and regret',
                     'actual selected plans', 'compact batch/ref integration',
                     'missing/stale/partial workload measurements'],
    }))
    raise SystemExit(1)
