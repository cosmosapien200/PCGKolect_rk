"""Open a PCGKolect session in Audacity with two label tracks: Beats and Quality.

Run from anywhere (lives in PCGKolect_rk/tools/):
  python tools/view_in_audacity.py                   # menu of recordings in PCGKolect_rk/recordings/
  python tools/view_in_audacity.py -s 001            # session_001 (also: -s 1, -s session_001)
  python tools/view_in_audacity.py -s 2 --labels-only           # just write the .txt label files
  python tools/view_in_audacity.py path/to/any.wav --recompute  # any WAV; labels computed on the PC
  --dir DIR picks another recordings folder.

Input  : session_NNN.wav + session_NNN_beats.csv + session_NNN_quality.csv (written by the firmware).
Tracks : 'Beats'   one label per decoded beat:  OK 0.93 | REJ 0.61 | GATE 0.88
         'Quality' merged 1 s states: CLEAN | NOISE | NO_CONTACT | TOO_LOUD | CLIPPING
Also writes <wav>_beats_labels.txt / <wav>_quality_labels.txt (Audacity label format).

Audacity is driven through mod-script-pipe: the script starts Audacity if needed (or reuses an open
one), imports the WAV and both label files. Every step has a timeout, so it never hangs; if Audacity
shows a dialog, close it and re-run. Enable the pipe once with --enable-pipe (Audacity closed), or
Edit > Preferences > Modules > mod-script-pipe = Enabled, then restart Audacity.
"""
import argparse, csv, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
RK_ROOT = os.path.dirname(HERE)                                         # PCGKolect_rk
ROOT = os.path.normpath(os.path.join(RK_ROOT, '..', 'PCGKolect_model'))   # model repo (only for --recompute)
CFG = os.path.expanduser('~/.config/audacity/audacity.cfg')
UID = os.getuid()
TO_PIPE = f'/tmp/audacity_script_pipe.to.{UID}'
FROM_PIPE = f'/tmp/audacity_script_pipe.from.{UID}'
GATES = ('NO_CONTACT', 'TOO_LOUD', 'CLIPPING')


# ----------------------------------------------------------------------------- labels

def beats_labels(rows):
    out = []
    for r in rows:
        tag = 'GATE' if int(r['gated']) else ('OK' if int(r['usable']) else 'REJ')
        out.append((float(r['t_start_s']), float(r['t_end_s']), f"{tag} {float(r['conf']):.2f}"))
    return out


def quality_state(r):
    if r['v1_reason'] in GATES:
        return r['v1_reason']
    pn = float(r['v2_pnoise'])
    if pn >= 0:                                   # v2 (NPU) available
        return 'NOISE' if pn > 0.5 else 'CLEAN'
    return 'CLEAN' if r['v1_verdict'] == 'GOOD' else 'NOISE'   # v1-only session


def quality_labels(rows):
    """Each row covers a 2 s span with 1 s hop -> use its centre second, then merge equal states."""
    segs = []
    for r in rows:
        t0, t1 = float(r['t_start_s']) + 0.5, float(r['t_end_s']) - 0.5
        st = quality_state(r)
        if segs and segs[-1][2] == st and abs(segs[-1][1] - t0) < 1e-6:
            segs[-1][1] = t1
            segs[-1][3].append(r)
        else:
            segs.append([t0, t1, st, [r]])
    out = []
    for t0, t1, st, rs in segs:
        lvl = sum(float(r['level_dbfs']) for r in rs) / len(rs)
        pn = [float(r['v2_pnoise']) for r in rs if float(r['v2_pnoise']) >= 0]
        extra = f' pN={sum(pn) / len(pn):.2f}' if pn else f" q={sum(float(r['v1_q']) for r in rs) / len(rs):.2f}"
        out.append((t0, t1, f'{st}{extra} {lvl:.0f}dBFS'))
    return out


def write_labels(path, labels):
    with open(path, 'w') as f:
        for s, e, t in labels:
            f.write(f'{s:.3f}\t{e:.3f}\t{t}\n')


def recompute(wav):
    """Run the device algorithms on the PC (v1 per second + v2 streaming beats) for a bare WAV."""
    if not os.path.isdir(ROOT):
        sys.exit(f'--recompute needs the PCGKolect_model repo next to PCGKolect_rk (looked in {ROOT}).')
    sys.path.insert(0, ROOT); sys.path.insert(0, os.path.join(ROOT, 'seg'))
    try:
        import numpy as np, soundfile as sf, onnxruntime as ort  # noqa: F401
    except ImportError as e:
        sys.exit(f'{e}. This WAV has no device CSVs, so labels must be recomputed on the PC, which needs\n'
                 f'  pip install numpy scipy soundfile onnxruntime\n'
                 f'(device sessions with _beats.csv/_quality.csv work with plain python).')
    from scipy.signal import resample_poly
    from sqi_frontend import score_stream
    from stream import stream_beats
    pcm, fs = sf.read(wav, dtype='int16')
    if pcm.ndim > 1:
        pcm = pcm[:, 0]
    if fs != 8000:
        pcm = np.clip(np.round(resample_poly(pcm.astype(np.float64), 8000, fs)), -32768, 32767).astype(np.int16)
    sess = ort.InferenceSession(os.path.join(ROOT, 'release', 'segnet.onnx'))
    frame_noise = {}

    def infer(feat):
        z = sess.run(None, {'feat': feat[None, :, None, :].astype(np.float32)})[0][0, :, 0].T
        e = np.exp(z - z.max(1, keepdims=True))
        return (e / e.sum(1, keepdims=True)).astype(np.float32)

    beats = stream_beats(pcm, infer)
    # per-frame P(noise) for the quality track (256-frame windows, hop 128 frames)
    from sqi_frontend import device_to_2k
    from stream import features
    x = device_to_2k(pcm).astype(np.float32)
    pn = np.zeros(len(x) // 40 + 1); cnt = np.zeros_like(pn)
    for s in range(0, max(len(x) - 10328 + 1, 0), 128 * 40):
        p = infer(features(x[s:s + 10328]).astype(np.float32))
        pn[s // 40:s // 40 + 256] += p[:, 4]; cnt[s // 40:s // 40 + 256] += 1
    pn /= np.maximum(cnt, 1)
    brows = [dict(t_start_s=b['start'] / 50, t_end_s=b['end'] / 50, conf=b['conf'], pnoise=b['pnoise'],
                  usable=int(b['usable']), gated=0) for b in beats]
    qrows = []
    for r in score_stream(pcm):
        f0, f1 = int(r['t_start'] * 50), int(r['t_end'] * 50)
        gated = r['reason'] in GATES
        qrows.append(dict(t_start_s=r['t_start'], t_end_s=r['t_end'], v1_q=r['q'], v1_verdict=r['verdict'],
                          v1_reason=r['reason'], level_dbfs=r['level_dbfs'],
                          v2_pnoise=float(pn[f0:f1].mean()) if f1 > f0 else -1, v2_conf=-1))
        if gated:
            for b in brows:
                if b['t_end_s'] > r['t_start'] + 0.5 and b['t_start_s'] < r['t_end'] - 0.5:
                    b['gated'], b['usable'] = 1, 0
    return brows, [{k: str(v) for k, v in q.items()} for q in qrows]


# ----------------------------------------------------------------------------- Audacity pipe

def enable_pipe():
    if subprocess.run(['pgrep', '-x', 'audacity'], capture_output=True).returncode == 0:
        sys.exit('Close Audacity first, then re-run with --enable-pipe.')
    txt = open(CFG).read()
    new = re.sub(r'^mod-script-pipe=\d+$', 'mod-script-pipe=1', txt, count=1, flags=re.M)
    if new == txt and 'mod-script-pipe=1' not in txt:
        sys.exit(f'Could not find mod-script-pipe in {CFG}; enable it in Preferences > Modules.')
    shutil.copy(CFG, CFG + '.bak')
    open(CFG, 'w').write(new)
    print(f'mod-script-pipe enabled in {CFG} (backup: {CFG}.bak)')


class Pipe:
    """mod-script-pipe client with non-blocking I/O and per-command timeouts (never hangs)."""

    def __init__(self, timeout=15):
        import errno
        self.timeout = timeout
        try:
            self.wfd = os.open(TO_PIPE, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as e:
            if e.errno == errno.ENXIO:
                raise ConnectionError('stale pipe: no Audacity is reading it')
            raise
        self.rfd = os.open(FROM_PIPE, os.O_RDONLY | os.O_NONBLOCK)
        self.buf = b''

    def cmd(self, c, timeout=None):
        import select
        os.write(self.wfd, (c + '\n').encode())
        deadline = time.time() + (timeout or self.timeout)
        while True:
            end = self.buf.find(b'\n\n')
            if end >= 0 and b'BatchCommand finished' in self.buf[:end]:
                res, self.buf = self.buf[:end].decode(errors='replace'), self.buf[end + 2:]
                if 'BatchCommand finished: OK' not in res:
                    raise RuntimeError(f'{c!r} -> {res}')
                return res
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError(f'Audacity did not answer {c.split(":")[0]!r} within {timeout or self.timeout} s. '
                                   'Is a dialog open in Audacity? Close it and re-run.')
            r, _, _ = select.select([self.rfd], [], [], min(left, 0.5))
            if r:
                chunk = os.read(self.rfd, 65536)
                if chunk:
                    self.buf += chunk


def audacity_running():
    return subprocess.run(['pgrep', '-x', 'audacity'], capture_output=True).returncode == 0


def connect(timeout=40):
    """Connect to a running Audacity, or start one and wait (bounded) for its scripting pipe."""
    if audacity_running() and os.path.exists(TO_PIPE):
        try:
            p = Pipe()
            p.cmd('GetInfo: Type=Commands Format=Brief', timeout=10)
            return p
        except (ConnectionError, TimeoutError) as e:
            sys.exit(f'Audacity is open but not responding to scripting ({e}).')
    if audacity_running():
        sys.exit('Audacity is open but its scripting pipe is off. Enable mod-script-pipe '
                 '(close Audacity, run with --enable-pipe) and try again.')
    print('Starting Audacity ...')
    subprocess.Popen(['audacity'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout:
        time.sleep(1.0)
        if not os.path.exists(TO_PIPE):
            continue
        try:
            p = Pipe()
            p.cmd('GetInfo: Type=Commands Format=Brief', timeout=5)
            return p
        except (ConnectionError, TimeoutError, OSError) as e:
            last = e                                    # still starting up / dialog open: keep waiting
    sys.exit(f'Started Audacity but could not talk to it within {timeout} s ({last}).\n'
             'If a welcome/first-run dialog is showing, close it and re-run this script '
             '(it will reuse the open Audacity). If not, enable the pipe: --enable-pipe.')


def track_names(p):
    import json
    info = p.cmd('GetInfo: Type=Tracks Format=JSON')
    js = info[:info.rfind(']') + 1]
    return [t.get('name', '') for t in json.loads(js)]


def add_label_track(p, name, labels):
    """New label track built label by label (Import2 on a .txt opens a dialog in Audacity 3.7)."""
    p.cmd('NewLabelTrack:')
    idx = len(track_names(p)) - 1
    p.cmd(f'SetTrack: Track={idx} Name="{name}"')
    info = p.cmd('GetInfo: Type=Labels Format=JSON')
    first = len(re.findall(r'\[\s*-?[\d.]+\s*,\s*-?[\d.]+\s*,\s*"', info))   # labels already in project
    for i, (s, e, text) in enumerate(labels):
        p.cmd(f'SelectTracks: Track={idx} TrackCount=1 Mode=Set')
        p.cmd(f'SelectTime: Start={s:.3f} End={e:.3f}')
        p.cmd('AddLabel:')
        p.cmd(f'SetLabel: Label={first + i} Text="{text}" Start={s:.3f} End={e:.3f}')


DEFAULT_DIR = os.path.join(RK_ROOT, 'recordings')


def wav_info(path):
    import wave
    try:
        with wave.open(path) as w:
            return w.getnframes() / w.getframerate()
    except Exception:
        return None


def list_recordings(d):
    if not os.path.isdir(d):
        sys.exit(f'Recordings folder not found: {d}')
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.lower().endswith('.wav'))


def resolve_session(d, s):
    m = re.fullmatch(r'(?:session_)?(\d+)', s.strip())
    if not m:
        sys.exit(f'Bad session number {s!r} (use e.g. 001, 2 or session_003)')
    path = os.path.join(d, f'session_{int(m.group(1)):03d}.wav')
    if not os.path.exists(path):
        have = [os.path.basename(f) for f in list_recordings(d)]
        sys.exit(f'{path} not found. Available: {", ".join(have) or "none"}')
    return path


def menu(d):
    files = list_recordings(d)
    if not files:
        sys.exit(f'No .wav recordings in {d} (run "make pull-recordings").')
    print(f'Recordings in {d}:')
    for i, f in enumerate(files, 1):
        base = os.path.splitext(f)[0]
        dur = wav_info(f)
        csvs = os.path.exists(base + '_beats.csv') and os.path.exists(base + '_quality.csv')
        extra = ''
        if csvs:
            rows = list(csv.DictReader(open(base + '_beats.csv')))
            extra = f'{len(rows):4d} beats, {sum(int(r["usable"]) for r in rows):4d} usable'
        else:
            extra = 'no device CSVs (needs --recompute + numpy)'
        print(f'  {i:2d}) {os.path.basename(f):28s} {dur or 0:7.1f} s   {extra}')
    while True:
        try:
            c = input(f'Choose 1-{len(files)} (q to quit): ').strip()
        except (EOFError, KeyboardInterrupt):
            sys.exit('\ncancelled')
        if c.lower() in ('q', 'quit', ''):
            sys.exit('cancelled')
        if c.isdigit() and 1 <= int(c) <= len(files):
            return files[int(c) - 1]
        print('  invalid choice')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('wav', nargs='?', help='path to a WAV (optional; otherwise -s or the menu)')
    ap.add_argument('-s', '--session', help='session number, e.g. 001 -> <dir>/session_001.wav')
    ap.add_argument('--dir', default=DEFAULT_DIR, help=f'recordings folder (default {DEFAULT_DIR})')
    ap.add_argument('--recompute', action='store_true', help='ignore/without device CSVs: compute labels on the PC')
    ap.add_argument('--labels-only', action='store_true', help='only write the label .txt files')
    ap.add_argument('--enable-pipe', action='store_true', help='enable mod-script-pipe in audacity.cfg')
    a = ap.parse_args()
    if a.enable_pipe:
        enable_pipe()
        if not a.wav and not a.session:
            return
    if a.wav:
        wav = os.path.abspath(a.wav)
    elif a.session:
        wav = resolve_session(a.dir, a.session)
    else:
        wav = menu(a.dir)
    wav = os.path.abspath(wav)
    if not os.path.exists(wav):
        sys.exit(f'File not found: {wav}')
    base = os.path.splitext(wav)[0]
    bcsv, qcsv = base + '_beats.csv', base + '_quality.csv'
    if not a.recompute and os.path.exists(bcsv) and os.path.exists(qcsv):
        brows = list(csv.DictReader(open(bcsv)))
        qrows = list(csv.DictReader(open(qcsv)))
        src = 'device CSVs'
    else:
        print('No device CSVs (or --recompute): running the v1 + v2 pipeline on the PC ...')
        brows, qrows = recompute(wav)
        src = 'PC recompute'
    bl, ql = beats_labels(brows), quality_labels(qrows)
    write_labels(base + '_beats_labels.txt', bl)
    write_labels(base + '_quality_labels.txt', ql)
    n_ok = sum(1 for r in brows if int(r['usable']))
    print(f'{os.path.basename(wav)} ({src}): {len(brows)} beats, {n_ok} usable; '
          f'{len(ql)} quality spans -> {base}_beats_labels.txt, {base}_quality_labels.txt')
    if a.labels_only:
        return
    p = connect()
    try:
        stem = os.path.splitext(os.path.basename(wav))[0]
        names = track_names(p)
        if names == [stem]:                      # this recording is already open alone: just add labels
            pass
        else:
            if names:                            # something else open: use a fresh project window
                p.cmd('New:', timeout=20)
                time.sleep(1.0)
            p.cmd(f'Import2: Filename="{wav}"', timeout=30)
        add_label_track(p, 'Beats', bl)
        add_label_track(p, 'Quality', ql)
        p.cmd('SelectTracks: Track=0 TrackCount=1 Mode=Set')
        p.cmd('SelectTime: Start=0 End=0')
    except (TimeoutError, RuntimeError) as e:
        sys.exit(f'Audacity scripting failed: {e}')
    print('Loaded into Audacity: audio + "Beats" + "Quality" label tracks.')


if __name__ == '__main__':
    main()
