# Stress test for fakechroot thread safety: 8 threads open/stat/readlink/listdir files concurrently.
import glob
import os
import threading

files = glob.glob("/usr/share/omarchy/**/*.qml", recursive=True)
links = [p for p in glob.glob("/usr/lib/*.so*")[:300] if os.path.islink(p)]
errors = []


def work(seed):
    try:
        for i in range(30):
            for f in files[seed::8]:
                os.stat(f)
                with open(f) as fh:
                    for _ in fh:
                        pass
            for l in links[seed::8]:
                os.readlink(l)
                os.path.realpath(l)
            os.listdir("/usr/share/omarchy/shell")
    except Exception as e:  # noqa: BLE001
        errors.append(repr(e))


threads = [threading.Thread(target=work, args=(s,)) for s in range(8)]
for t in threads:
    t.start()
for t in threads:
    t.join()
print("threads done, errors:", errors[:3])
