"""Executed by GDB's Python, with GGC_VM_CONTROL pointing at a case directory."""
import json
import os
from pathlib import Path
import time

import gdb

directory = Path(os.environ["GGC_VM_CONTROL"])


def write(name, value):
    temp = directory / (name + ".tmp")
    temp.write_text(json.dumps(value, indent=2) + "\n")
    temp.replace(directory / name)


def await_file(name):
    deadline = time.monotonic() + 90
    while not (directory / name).exists():
        if time.monotonic() > deadline:
            raise RuntimeError("timed out waiting for " + name)
        time.sleep(.02)


class VertexBoundary(gdb.Breakpoint):
    def __init__(self, config):
        super().__init__(config["symbol"], internal=True)
        self.config = config
        self.hits = 0

    def stop(self):
        top = gdb.newest_frame()
        if self.config["symbol"] == "visibilitymap_get_status":
            rel = top.read_var("rel")
        else:
            rel = top.read_var("scan").dereference()["heapRelation"]
        if int(rel.dereference()["rd_id"]) != self.config["relation"]:
            return False
        frame = top
        while frame and frame.name() not in ("vertex_visible", "flush_pending_vertices"):
            frame = frame.older()
        if frame is None:
            return False
        self.hits += 1
        if self.hits != self.config["stop_after"]:
            return False
        record = dict(symbol=self.config["symbol"], hits=self.hits,
                      caller=frame.name(), backtrace=gdb.execute("bt", to_string=True))
        # These fields are evidence, not a debugger mutation of the scan.
        if self.config["symbol"] == "index_fetch_heap":
            scan = top.read_var("scan")
        else:
            record["heap_block"] = int(top.read_var("heapBlk"))
            # The VM buffer address is a non-optimized function argument and
            # names this owner's field even when the caller's local is gone.
            offset = int(gdb.parse_and_eval("(unsigned long)&((GgcOwnedFixedStage *)0)->vertex_vm_buffer"))
            owner = gdb.Value(int(top.read_var("vmbuf")) - offset).cast(
                gdb.lookup_type("GgcOwnedFixedStage").pointer())
            scan = owner.dereference()["vertex_scan"]
        scan = scan.dereference()
        opaque = scan["opaque"].cast(gdb.lookup_type("BTScanOpaqueData").pointer()).dereference()
        record.update(xs_want_itup=bool(scan["xs_want_itup"]),
                      number_of_keys=int(scan["numberOfKeys"]),
                      drop_pin=bool(opaque["dropPin"]),
                      leaf_buffer=int(opaque["currPos"]["buf"]))
        if "heap_block" not in record:
            tid = scan["xs_heaptid"]["ip_blkid"]
            record["heap_block"] = (int(tid["bi_hi"]) << 16) | int(tid["bi_lo"])
        if frame.name() == "vertex_visible":
            try:
                record["key"] = int(frame.read_var("key"))
            except gdb.error:
                pass
        write("paused.json", record)
        return True


attached = False
try:
    await_file("attach.json")
    config = json.loads((directory / "attach.json").read_text())
    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    gdb.execute("set print thread-events off")
    gdb.execute("attach " + str(config["pid"]))
    attached = True
    breakpoint = VertexBoundary(config)
    write("armed.json", dict(pid=config["pid"]))
    gdb.execute("continue")
    if not (directory / "paused.json").exists():
        raise RuntimeError("backend stopped outside the requested graph boundary")
    await_file("release")
    breakpoint.delete()
    gdb.execute("detach")
    attached = False
    write("done.json", dict(status="PASS"))
except BaseException as exc:
    write("gdb_error.json", dict(error=repr(exc)))
    if attached:
        try:
            gdb.execute("detach")
        except gdb.error:
            pass
    raise
