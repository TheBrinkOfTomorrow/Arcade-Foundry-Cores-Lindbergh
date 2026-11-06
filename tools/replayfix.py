#!/usr/bin/env python3
"""Replays cmake/gl4es_fixes.cmake on a pristine gl4es, entry by entry in order, as
gl4es_fix does (every "from" replaced by "to"; one that is not there stops the configure).
Run it after any merge that touches cmake/gl4es_fixes.cmake: an entry whose text an
earlier entry has changed matches nothing, and the configure would stop on it.

  tools/replayfix.py cmake/gl4es_fixes.cmake <pristine gl4es> [<patched gl4es>]

<pristine gl4es> is the pinned commit extracted (the URL in CMakeLists.txt);
<patched gl4es> a build's build/_deps/gl4es-src, to compare the files that come out.
Lists the entries that match nothing at their turn; exits 1 if any did, or if a
file comes out other than in <patched gl4es>.
"""
import sys, os
if len(sys.argv) not in (3, 4):
    sys.exit(__doc__)
cm, pristine = sys.argv[1:3]
patched = sys.argv[3] if len(sys.argv) == 4 else None
s = open(cm).read()
ESC = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\', ';': ';', '$': '$', '(': '(', ')': ')', '#': '#', ' ': ' '}
def arg(i):   # one argument from s[i]: "quoted" or [=[bracket]=]; returns (value, next index)
    while s[i] in ' \t\n': i += 1
    if s[i] == '"':
        out, i = [], i + 1
        while s[i] != '"':
            if s[i] == '\\': out.append(ESC.get(s[i + 1], '\\' + s[i + 1])); i += 2
            else: out.append(s[i]); i += 1
        return ''.join(out), i + 1
    assert s[i] == '[', s[i:i + 40]
    j = i + 1
    while s[j] == '=': j += 1
    eq = s[i + 1:j]; assert s[j] == '['
    end = s.index(']' + eq + ']', j + 1)
    v = s[j + 1:end]
    return (v[1:] if v.startswith('\n') else v), end + len(eq) + 2
ents, i = [], 0
while True:
    i = s.find('\ngl4es_fix(', i)
    if i < 0: break
    i += len('\ngl4es_fix(')
    f, i = arg(i); a, i = arg(i); b, i = arg(i)
    ents.append((f, a, b))
files, skipped = {}, []
for k, (f, A, B) in enumerate(ents, 1):
    rel = f.replace('${gl4es_SOURCE_DIR}/', '')
    if rel not in files: files[rel] = open(os.path.join(pristine, rel)).read()
    t = files[rel]
    if A in t: files[rel] = t.replace(A, B)
    else: skipped.append((k, rel, A.split('\n')[0]))
for k, rel, line in skipped: print('entry', k, rel, 'matches nothing:', repr(line))
diff = [rel for rel, t in files.items() if patched and t != open(os.path.join(patched, rel)).read()]
print(len(ents), 'entries,', len(skipped), 'skipped' + ('; files other than in the build: ' + (', '.join(diff) or 'none') if patched else ''))
sys.exit(1 if skipped or diff else 0)
