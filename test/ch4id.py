import json, os, subprocess, sys, time

SQLITE = '/opt/homebrew/opt/sqlite/bin/sqlite3'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB = os.path.join(ROOT, 'test', 'ch4id.db')


def sql(*statements, db=DB, load=False):
    args = [SQLITE, db]
    if load:
        args += ['-cmd', '.load %s/ext/bric' % ROOT]
    return subprocess.run(args, input='\n'.join(statements),
                          capture_output=True, text=True)


def seed(keys):
    return sql('.read %s/example/ch4id.sql' % ROOT,
               'insert or replace into plume (key) values %s;'
               % ', '.join("('%s')" % k for k in keys), db=DB,
               load=True)


def wait(keys, seconds=1800):
    start = time.time()
    while time.time() - start < seconds:
        r = sql('select key, source_kind, confidence from plume_source '
                'where key in (%s) order by key;' % ', '.join("'%s'" % k for k in keys),
                'select key, kind, coalesce(detail, "") from bric_log '
                "where kind in ('error') order by seq desc limit 5;")
        rows = [l for l in r.stdout.splitlines() if '|' in l]
        done = [l for l in rows if l.split('|')[0] in keys]
        if len(done) >= len(keys):
            return r.stdout
        time.sleep(10)
    return 'timeout\n' + sql('select key, kind, turn, detail from bric_log '
                             'order by seq desc limit 10;').stdout


PICKS = {
    'imeo-emit': 'IMEO:ae87ae10-a948-42c3-ba96-248157db65fe',
    'cm-tanager-waste': 'CM:tan20250101t113529c00s4001-A',
    'dd-aerial-satellite': 'DD:aoi:S2B_40SBJ_20241025_0_L1C:plume-1',
    'dd-barrow-pipeline': 'DD:pipeline-barrow:S2A_30UVE_20260314_1_L1C:plume-1',
}

if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'wait':
        print(wait(list(PICKS.values())))
    else:
        which = sys.argv[1:] or list(PICKS)
        print(seed([PICKS[w] for w in which]))
