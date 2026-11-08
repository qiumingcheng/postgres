#!/usr/bin/env python3
"""Exhaustive small-graph oracle. Does not share code with either DB executor."""
from __future__ import annotations
from collections import Counter
import json
from common import PACKAGE, read

def bag(rows):
    return Counter(json.dumps(r, separators=(',', ':'), ensure_ascii=False) for r in rows)

def enumerate_paths(graph, case):
    mode = case['mode']
    if mode not in ('WALK', 'TRAIL', 'ACYCLIC', 'SIMPLE'):
        raise ValueError('unsupported mode')
    lower, upper = case['lower'], case['upper']
    if not 0 <= lower <= upper <= 8:
        raise ValueError('oracle is deliberately bounded to 8 hops')
    source = case['source']
    if source not in graph['vertices']:
        return []
    direction = case.get('direction', 'OUT')
    adjacency = {v: [] for v in graph['vertices']}
    for eid, src, dst in graph['edges']:
        if direction in ('OUT', 'UNDIRECTED'):
            adjacency[src].append((eid, dst))
        if direction == 'IN' or (direction == 'UNDIRECTED' and src != dst):
            adjacency[dst].append((eid, src))
    results = []
    # Enumerate complete histories; never use the production visited/dominance rules.
    stack = [(source, [], [source])]
    while stack:
        vertex, edges, vertices = stack.pop()
        depth = len(edges)
        if depth >= lower and (case.get('target') is None or vertex == case['target']):
            results.append([source, edges, vertex])
        if depth == upper or (mode == 'SIMPLE' and depth and vertex == source):
            continue
        for eid, dst in adjacency[vertex]:
            if mode == 'TRAIL' and eid in edges:
                continue
            if mode == 'ACYCLIC' and dst in vertices:
                continue
            if mode == 'SIMPLE' and dst in vertices and dst != source:
                continue
            stack.append((dst, edges + [eid], vertices + [dst]))
    return results

def solve(graph, case):
    paths = enumerate_paths(graph, case)
    mode = case['result']
    if mode == 'REACHABLE_SET':
        return [list(p) for p in sorted({(r[0], r[2]) for r in paths})]
    if mode in ('ALL_SHORTEST', 'ANY_SHORTEST') and paths:
        length = min(len(r[1]) for r in paths)
        paths = [r for r in paths if len(r[1]) == length]
        if mode == 'ANY_SHORTEST':
            return sorted(paths)[:1]  # DB ANY outputs are checked for membership, not this tie choice.
    return paths

def selfcheck():
    doc = read(PACKAGE / 'golden/small_graphs.json')
    for case in doc['cases']:
        actual = solve(doc['graphs'][case['graph']], case)
        if bag(actual) != bag(case['expected']):
            raise AssertionError((case['id'], actual, case['expected']))
    print(f"PASS: {len(doc['cases'])} hand-authored oracle cases")
    return len(doc['cases'])

if __name__ == '__main__':
    selfcheck()
