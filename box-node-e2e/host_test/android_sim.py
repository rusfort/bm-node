#!/usr/bin/env python3
"""
Хост-тест автомата e2e_test.c против эмулятора Android.

Эмулятор повторяет правила FileSyncWorker.java (V1):
  * BOX  -> BoxUnpacker: новые сообщения -> transit + CBB(box); известные -> CDB(box);
           повтор box header (первые 44 символа) отбрасывается до распаковки;
           B += число новых сообщений.
  * CBB  -> C += len(box_ids); ACB(cbb_id), если box есть в своей очереди; refresh очереди.
  * CDB  -> D += len(box_ids); ACB(cdb_id), если box есть в своей очереди.
  * ACB  -> A += число реально удалённых ожидающих CBB/CDB.
  * Передающая сессия: SID, ACB, CBB, CDB, BOX..., TRM, TRM; CBB/CDB живут до ACB.

Запуск:  python3 android_sim.py  (собирает libe2e.so из ../main)
"""
import ctypes, os, random, subprocess, sys, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.join(HERE, "..", "main")
LIB = os.path.join(HERE, "libe2e.so")

SAN = ["-fsanitize=address,undefined"] if os.environ.get("E2E_ASAN") else []
subprocess.check_call([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                       *SAN, "-shared", "-fPIC",
                       "-I", MAIN, os.path.join(MAIN, "box_proto.c"),
                       os.path.join(MAIN, "e2e_test.c"), "-o", LIB])

NODE_SUFFIX = b"aabbccddeeff"
ANDROID_SUFFIX = "0123456789ab"


class Pkt(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int), ("len", ctypes.c_uint16), ("data", ctypes.c_char * 513)]


class Session(ctypes.Structure):
    _fields_ = [("sid", ctypes.c_char * 37), ("n", ctypes.c_int), ("pkt", Pkt * 8),
                ("n_acb", ctypes.c_int), ("acb_ids", (ctypes.c_char * 64) * 8),
                ("has_cbb", ctypes.c_bool), ("has_cdb", ctypes.c_bool)]


RAND = ctypes.CFUNCTYPE(ctypes.c_uint32)
NOW = ctypes.CFUNCTYPE(ctypes.c_uint32)
LOG = ctypes.CFUNCTYPE(None, ctypes.c_char_p)


class Env(ctypes.Structure):
    _fields_ = [("rand32", RAND), ("now_ms", NOW), ("log", LOG), ("node_suffix", ctypes.c_char_p),
                ("ttl", ctypes.c_char_p), ("timeout_ms", ctypes.c_uint32)]


def run(seed, drop_rate=0.0, android_extra_msgs=1, verbose=False, max_rounds=400):
    lib = ctypes.CDLL(LIB)
    lib.e2e_on_rx.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.e2e_build_session.argtypes = [ctypes.POINTER(Session), ctypes.c_uint16]
    lib.e2e_commit_session.argtypes = [ctypes.POINTER(Session), ctypes.c_int]
    rng = random.Random(seed)
    clock = [0]
    logs = []
    cb_r = RAND(lambda: rng.getrandbits(32))
    cb_n = NOW(lambda: clock[0])
    cb_l = LOG(lambda s: logs.append(s.decode()) or (verbose and print("NODE", s.decode())))
    env = Env(cb_r, cb_n, cb_l, NODE_SUFFIX, b"2026-12-31 23:59:59", 15 * 60 * 1000)
    lib.e2e_init(ctypes.byref(env))

    def auuid():
        u = str(uuid.UUID(int=rng.getrandbits(128), version=4))
        return u[:-12] + ANDROID_SUFFIX

    # --- состояние Android ---
    cnt = dict(B=0, C=0, D=0, A=0)
    known_msgs = set()
    transit = []                          # (mid, ttl, uc, recv, type, bytes)
    for i in range(android_extra_msgs):   # старые транзитные/свои сообщения на телефоне
        m = (str(uuid.UUID(int=rng.getrandbits(128), version=4)), "2099-12-31 23:59:59", 1,
             "ENC-RECV", "text", "OLD-%d" % i)
        transit.append(m); known_msgs.add(m[0])
    seen_headers = set()
    known_sessions = set()
    cbb_q, cdb_q, acb_q = [], [], []      # [(id, [boxes])], [(id,[boxes])], [ids]
    boxes = {}                            # box_id -> [mid]
    refresh = [True]
    sid = [str(uuid.uuid4())]

    def refill():
        boxes.clear()
        # Как queryDatabaseToDataBlock: несколько коротких сообщений в одном BOX
        cur = []
        for m in transit:
            cur.append(m)
            if len(cur) == 2:
                boxes[str(uuid.UUID(int=rng.getrandbits(128), version=4))] = cur; cur = []
        if cur:
            boxes[str(uuid.UUID(int=rng.getrandbits(128), version=4))] = cur
        refresh[0] = False

    def android_rx(data):
        lines = data.split("\n")
        while lines and lines[-1] == "":
            lines.pop()
        if len(lines) < 3 or lines[0] != "V1":
            return "cont"
        t = lines[1]
        if t == "SID":
            if lines[2] in known_sessions:
                return "close"
            android_rx.sid = lines[2]
        elif t == "TRM":
            if getattr(android_rx, "sid", None) == lines[2]:
                known_sessions.add(lines[2])
            return "close"
        elif t == "ACB":
            ids = lines[2:]
            found = 0
            for q in (cbb_q, cdb_q):
                for e in list(q):
                    if e[0] in ids:
                        q.remove(e); found += 1
            cnt["A"] += found
        elif t in ("CBB", "CDB"):
            cid, bids = lines[2], lines[3:]
            cnt["C" if t == "CBB" else "D"] += len(bids)
            if any(b in boxes for b in bids):
                if t == "CBB":
                    refresh[0] = True; sid[0] = str(uuid.uuid4())
                if cid not in acb_q:
                    acb_q.append(cid)
        elif t == "BOX":
            hdr = data[:44]
            if hdr in seen_headers:
                return "cont"
            seen_headers.add(hdr)
            body = lines[3:]
            new = dup = 0
            for i in range(0, len(body) - 5, 6):
                mid = body[i]
                if mid in known_msgs:
                    dup += 1
                else:
                    known_msgs.add(mid); new += 1
                    transit.append((mid, body[i + 1], int(body[i + 2]) + 1, body[i + 3], body[i + 4], body[i + 5]))
            cnt["B"] += new
            if new:
                cbb_q.append((auuid(), [lines[2]])); refresh[0] = True
            if dup:
                cdb_q.append((auuid(), [lines[2]]))
            sid[0] = str(uuid.uuid4())
        return "cont"

    def android_session():
        if refresh[0]:
            refill()
        out = ["V1\nSID\n" + sid[0]]
        if acb_q:
            out.append("V1\nACB\n" + "\n".join(acb_q)); acb_q.clear()
        for cid, b in cbb_q:
            out.append("V1\nCBB\n" + cid + "\n" + "\n".join(b))
        for cid, b in cdb_q:
            out.append("V1\nCDB\n" + cid + "\n" + "\n".join(b))
        for bid, msgs in boxes.items():
            s = "V1\nBOX\n" + bid + "\n"
            for m in msgs:
                s += "\n".join([m[0], m[1], str(m[2]), m[3], m[4], m[5]]) + "\n"
            out.append(s)
        out += ["V1\nTRM\n" + sid[0]] * 2
        return out

    for rnd in range(max_rounds):
        clock[0] += 15000
        # Фаза 1: Android подключается к Node (Node = GATT server)
        s = Session()
        n = lib.e2e_build_session(ctypes.byref(s), 514)
        delivered = 0
        android_rx.sid = None
        for i in range(n):
            if rng.random() < drop_rate:
                break                       # обрыв соединения
            delivered += 1
            if android_rx(s.pkt[i].data[: s.pkt[i].len].decode()) == "close":
                break
        lib.e2e_commit_session(ctypes.byref(s), delivered)
        # Фаза 2: Node подключается к Android (Android = GATT server)
        lib.e2e_rx_connection_begin()
        for p in android_session():
            if rng.random() < drop_rate:
                break
            if lib.e2e_on_rx(p.encode(), len(p.encode())) == 1:
                break
        lib.e2e_tick()
        res = lib.e2e_result()
        if res != 0:
            break
    # Дать узлу ещё несколько сессий, чтобы получить ACB от Android (информационно)
    return lib.e2e_result(), cnt, logs, rnd


if __name__ == "__main__":
    fails = 0
    cases = [(s, 0.0, 1) for s in range(20)] + [(s, 0.0, 3) for s in range(20, 30)] + \
            [(s, 0.15, 2) for s in range(30, 80)]
    for seed, drop, extra in cases:
        res, cnt, logs, rnd = run(seed, drop, extra)
        ok = res == 1 and cnt == dict(B=1, C=1, D=1, A=2)
        if not ok:
            fails += 1
            print("FAIL seed=%d drop=%.2f extra=%d res=%d cnt=%s rounds=%d" % (seed, drop, extra, res, cnt, rnd))
            print("\n".join(logs[-40:]))
    print("cases=%d fails=%d" % (len(cases), fails))
    # Пример полного лога одного прогона
    res, cnt, logs, rnd = run(1, 0.0, 1)
    print("\n--- sample run: result=%d android=%s rounds=%d ---" % (res, cnt, rnd))
    print("\n".join(l for l in logs if "STEP" in l or "PASS" in l))
    sys.exit(1 if fails else 0)
