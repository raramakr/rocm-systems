###############################################################################
# MIT License
#
# Copyright (c) 2023 Advanced Micro Devices, Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
###############################################################################

import os
import otf2
from otf2.enums import LocationType, LocationGroupType
import shutil
import time
import pdb


def write_otf2(importData, config):

    timer_resolution = 1_000_000
    trace_dir = getattr(config, "output_path", "./otf_traces")
    trace_file = f"{getattr(config, "output_file", "traces")}_results"
    if not os.path.exists(trace_dir):
        os.makedirs(trace_dir)

    print(trace_dir)
    if os.path.exists(trace_dir):
        shutil.rmtree(trace_dir)
    with otf2.writer.open(
        trace_dir, trace_file, timer_resolution=timer_resolution
    ) as archive:
        conn = getattr(importData, "connection", None)
        if conn is not None:
            try:
                cursor = conn.cursor()
                cursor.execute("SELECT MIN(start), MAX(fini) FROM processes;")
                min_start, max_finish = cursor.fetchone()

                with otf2.writer.DefinitionWriter(archive) as global_def_writer:
                    global_offset = min_start
                    duration = max_finish - min_start
                    realtime_timestamp = int(round(time.time() * timer_resolution))
                    global_def_writer.write_clock_properties(
                        timer_resolution, global_offset, duration, realtime_timestamp
                    )

                    cursor = conn.cursor()
                    cursor.execute("SELECT DISTINCT guid, id FROM rocpd_info_node")
                    for row in cursor:
                        guid, nid = row
                        cursor = conn.cursor()
                        cursor.execute(
                            "SELECT pid, hostname, command FROM processes WHERE guid = ? AND nid = ?",
                            (guid, nid),
                        )
                        for row in cursor:
                            pid, hostname, command = row
                            tree_node = archive.definitions.system_tree_node(
                                name=command, class_name=hostname, parent=None
                            )

                            cpu_location_group = archive.definitions.location_group(
                                name=command,
                                location_group_type=LocationGroupType.PROCESS,
                                system_tree_parent=tree_node,
                            )
                            cursor = conn.cursor()
                            cursor.execute(
                                "SELECT tid FROM threads WHERE guid = ? AND nid = ? AND pid = ?",
                                (guid, nid, pid),
                            )
                            for row in cursor:
                                tid = row[0]
                                location = archive.definitions.location(
                                    name=f"Thread {tid}",
                                    type=LocationType.CPU_THREAD,
                                    group=cpu_location_group,
                                )

                                event_writer = otf2.writer.EventWriter(archive, location)
                                events = []

                                cursor = conn.cursor()
                                cursor.execute(
                                    "SELECT start, end, region FROM kernels WHERE guid = ? AND nid = ? AND pid = ? AND tid = ? ORDER BY start ASC",
                                    (guid, nid, pid, tid),
                                )
                                for row in cursor:
                                    start, end, name = row
                                    region = archive.definitions.region(
                                        name=region,
                                    )
                                    events.append((start, "enter", region))
                                    events.append((end, "leave", region))

                                cursor = conn.cursor()
                                cursor.execute(
                                    "SELECT start, end, region FROM memory_copies WHERE guid = ? AND nid = ? AND pid = ? AND tid = ? ORDER BY start ASC",
                                    (guid, nid, pid, tid),
                                )
                                for row in cursor:
                                    region = archive.definitions.region()
                                    continue

                                events.sort(key=lambda x: x[0])

                                for ts, evt_type, region in events:
                                    if evt_type == "enter":
                                        event_writer.enter(ts, region)
                                    else:
                                        event_writer.leave(ts, region)
                print("Finished the loops")
            except Exception as e:
                print("Could not query sqlite database:", e)
        else:
            print("No sqlite connection found in importData.")


__all__ = ["write_otf2"]
