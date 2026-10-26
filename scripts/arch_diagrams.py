#!/usr/bin/env python3
"""Draw the architecture diagram of each server version -> assets/arch_*.png.

Needs matplotlib (.venv/bin/python scripts/arch_diagrams.py).
"""
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

OUT = Path(__file__).resolve().parent.parent / "assets"

C = {
    "client": "#dbe9fb", "io": "#e8e8e8", "reader": "#fde7c8", "adm": "#f9d3c0",
    "queue": "#d6efd9", "commit": "#f4b6b6", "worker": "#e6dcf5", "oracle": "#f3f3f3",
}


def box(ax, x, y, w, h, text, color, size=9, bold=False):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02,rounding_size=0.12",
                                fc=color, ec="#444", lw=1.1))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center", fontsize=size,
            fontweight="bold" if bold else "normal", linespacing=1.35)


def arrow(ax, x1, y1, x2, y2, label=None, dy=0.18):
    ax.annotate("", xy=(x2, y2), xytext=(x1, y1),
                arrowprops=dict(arrowstyle="-|>", color="#333", lw=1.3))
    if label:
        ax.text((x1 + x2) / 2, (y1 + y2) / 2 + dy, label, ha="center", fontsize=7.5, color="#333")


def reply_path(ax, x):
    """Reply frames travel back to the client's receiver thread under the server box."""
    ax.plot([x, x, 1.25, 1.25], [2.6, 1.35, 1.35, 1.75 - 0.02], color="#333", lw=1.3)
    ax.annotate("", xy=(1.25, 1.75), xytext=(1.25, 1.4),
                arrowprops=dict(arrowstyle="-|>", color="#333", lw=1.3))
    ax.text(8.0, 1.4, "RES|id|seq|status|reason|server_us", fontsize=7.5, color="#333", ha="center")


def base(title, subtitle):
    fig, ax = plt.subplots(figsize=(15, 6.6))
    ax.set_xlim(0, 15)
    ax.set_ylim(0, 6.6)
    ax.axis("off")
    ax.text(0.1, 6.35, title, fontsize=15, fontweight="bold")
    ax.text(0.1, 6.0, subtitle, fontsize=9.5, color="#444")
    # process boundaries
    ax.add_patch(FancyBboxPatch((0.1, 1.55), 2.3, 4.1, boxstyle="round,pad=0.02,rounding_size=0.1",
                                fc="none", ec="#2a6fdb", lw=1.4, ls="--"))
    ax.text(1.25, 5.45, "client processes", ha="center", fontsize=8.5, color="#2a6fdb")
    ax.add_patch(FancyBboxPatch((3.35, 1.55), 11.55, 4.1, boxstyle="round,pad=0.02,rounding_size=0.1",
                                fc="none", ec="#c4521f", lw=1.4, ls="--"))
    ax.text(9.1, 5.45, "ffcsync_server process: the only owner of registration state",
            ha="center", fontsize=8.5, color="#c4521f")
    # socket
    ax.add_patch(FancyBboxPatch((2.55, 1.55), 0.6, 4.1, boxstyle="round,pad=0.02", fc="#eee", ec="#888"))
    ax.text(2.85, 3.6, "Unix domain stream socket\n(4-byte length + payload)", rotation=90,
            ha="center", va="center", fontsize=8, color="#444")
    # oracle
    box(ax, 3.35, 0.2, 11.55, 0.9,
        "ffcsync_seq (separate process, offline): replays admission.csv in seq order  →  "
        "diff results.csv and final_state.csv  =  correctness oracle", C["oracle"], 9)
    return fig, ax


def clients(ax, burst):
    box(ax, 0.25, 3.3, 2.0, 1.7,
        "ffcsync_client\nsender thread\n(window: --max-inflight)\n" + ("sends a burst per write()" if burst else "one send() per request"),
        C["client"], 8)
    box(ax, 0.25, 1.75, 2.0, 1.2, "receiver thread\nmatches replies\nby request_id", C["client"], 8)
    arrow(ax, 2.25, 4.15, 3.35, 4.15)


def reader(ax, buffered):
    t = ("reader thread\n(one per connection)\n"
         + ("FrameReader: 64 KB read(),\nall complete frames per syscall\n" if buffered
            else "read_frame(): 2 read() calls\nper frame\n")
         + "decode; MALFORMED → seq 0")
    box(ax, 3.55, 3.3, 2.4, 1.7, t, C["reader"], 8)


def admission(ax, batch):
    box(ax, 6.25, 3.75, 2.3, 1.4,
        "ADMISSION\n(admission_mu_)\nseq = next_seq_++\nlog + queue.push()" + ("\nbatch per read" if batch else ""),
        C["adm"], 7.5, True)
    box(ax, 6.25, 2.6, 2.3, 1.0, "BoundedQueue (FIFO)\nback-pressure when full", C["queue"], 8)
    arrow(ax, 5.95, 4.45, 6.25, 4.45)
    arrow(ax, 7.4, 3.75, 7.4, 3.6)


def v1():
    fig, ax = base("Version 1: gate mode  (main, 2026-10-23)",
                   "Workers check static rules in parallel; CommitGate lets one worker commit at a time, in seq order. (--workers 0 = inline)")
    clients(ax, False)
    reader(ax, False)
    admission(ax, False)
    box(ax, 8.95, 3.6, 2.4, 1.4, "worker pool (N threads)\npop job (seq order)\ncheck_static()\nruns in parallel", C["worker"], 8)
    box(ax, 8.95, 1.75, 2.4, 1.4, "CommitGate.run_in_order(seq)\nwait until next_ == seq\ndecide_and_commit()\nnotify_all()", C["commit"], 7.5, True)
    box(ax, 11.75, 2.6, 3.0, 1.9,
        "reply\ncompletion callback sends\nRES|id|seq|status|reason\none send() per request,\noutside every lock\n(replies may be out of seq order)", C["io"], 8)
    arrow(ax, 8.55, 3.1, 8.95, 4.1)
    arrow(ax, 10.15, 3.6, 10.15, 3.15)
    arrow(ax, 11.35, 2.45, 11.75, 3.2)
    reply_path(ax, 13.25)
    return fig


def v2():
    fig, ax = base("Version 2: single-committer mode  (--mode committer)",
                   "One committer thread drains the queue in batches and commits every job in seq order: no gate, no worker hand-offs.")
    clients(ax, False)
    reader(ax, False)
    admission(ax, False)
    box(ax, 8.95, 2.0, 2.9, 3.0,
        "COMMITTER THREAD (exactly one)\npop_batch(): every queued job, FIFO\nunder commit_mu_, for each job:\n  check_static()\n  decide_and_commit()\nstate + results log written\nonly by this thread", C["commit"], 7.5, True)
    box(ax, 12.15, 2.6, 2.6, 1.9,
        "reply\nappend reply to outbox\nsend RES per request\n(one send() each)\nreplies in seq order", C["io"], 8)
    arrow(ax, 8.55, 3.1, 8.95, 3.4)
    arrow(ax, 11.85, 3.5, 12.15, 3.5)
    reply_path(ax, 13.45)
    return fig


def v3():
    fig, ax = base("Version 3: single committer + buffered socket I/O  (recommended)",
                   "Same commit logic as version 2, fewer syscalls: bursts in, 64 KB reads, batch admission, one write per connection per commit batch.")
    clients(ax, True)
    reader(ax, True)
    admission(ax, True)
    box(ax, 8.95, 2.0, 2.9, 3.0,
        "COMMITTER THREAD (exactly one)\npop_batch(): every queued job, FIFO\nunder commit_mu_, for each job:\n  check_static()\n  decide_and_commit()\nstate + results log written\nonly by this thread", C["commit"], 7.5, True)
    box(ax, 12.15, 2.6, 2.6, 1.9,
        "reply\nappend frame to each\nconnection's outbox\nflush_replies():\none write() per connection\nper batch (write_mu)", C["io"], 8)
    arrow(ax, 8.55, 3.1, 8.95, 3.4)
    arrow(ax, 11.85, 3.5, 12.15, 3.5)
    reply_path(ax, 13.45)
    return fig


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    for name, fn in (("arch_v1_gate", v1), ("arch_v2_committer", v2), ("arch_v3_committer_buffered", v3)):
        fig = fn()
        fig.savefig(OUT / f"{name}.png", dpi=110, bbox_inches="tight", facecolor="white")
        plt.close(fig)
        print("wrote", OUT / f"{name}.png")
