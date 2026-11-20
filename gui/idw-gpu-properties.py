#!/usr/bin/env python3
"""IDW live Intel GPU monitor. Reads kernel DRM/sysfs; never fabricates metrics."""
import json
import os
import shutil
import subprocess
import tkinter as tk
from tkinter import ttk, messagebox
from pathlib import Path

REFRESH_MS = 1500
PRIVILEGED_HELPER = "/usr/local/libexec/idw-debugfs-snapshot"


def find_idwctl():
    candidates = [
        shutil.which("idwctl"),
        "/usr/local/bin/idwctl",
        str(Path.home() / ".local/bin/idwctl"),
        str(Path(__file__).resolve().parent.parent / "build/idwctl"),
    ]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def read_text(path):
    try:
        return Path(path).read_text(encoding="ascii").strip()
    except (OSError, UnicodeError):
        return None


def read_first(paths):
    for path in paths:
        value = read_text(path)
        if value is not None:
            return value
    return None


def as_mhz(value):
    if value is None:
        return "Not exposed"
    try:
        return f"{int(value):,} MHz"
    except (TypeError, ValueError):
        return "Not exposed"


def sysfs_metrics(adapter):
    """Match DRM card sysfs to the PCI path reported by idwctl."""
    pci = (adapter.get("pci_path") or "").replace("0000:", "")
    result = {"frequency": {}, "memory": {}, "power": {}}
    if not pci:
        return result

    drm_root = Path("/sys/class/drm")
    matched = None
    try:
        for card in drm_root.glob("card[0-9]*"):
            device_link = card / "device"
            try:
                resolved = os.path.realpath(device_link)
            except OSError:
                continue
            if pci in resolved:
                matched = card
                break
    except OSError:
        pass

    if matched is None:
        return result

    dev = matched / "device"
    # Legacy i915 names used by Haswell, plus newer i915 paths as fallbacks.
    candidates = {
        "Actual frequency": [dev / "gt_act_freq_mhz", dev / "gt/gt0/rps_act_freq_mhz"],
        "Requested frequency": [dev / "gt_cur_freq_mhz", dev / "gt/gt0/rps_cur_freq_mhz"],
        "Minimum frequency": [dev / "gt_min_freq_mhz", dev / "gt/gt0/rps_min_freq_mhz"],
        "Maximum frequency": [dev / "gt_max_freq_mhz", dev / "gt/gt0/rps_max_freq_mhz"],
        "Hardware maximum": [dev / "gt_RP0_freq_mhz", dev / "gt/gt0/rps_RP0_freq_mhz"],
        "Hardware minimum": [dev / "gt_RPn_freq_mhz", dev / "gt/gt0/rps_RPn_freq_mhz"],
    }
    for label, paths in candidates.items():
        result["frequency"][label] = as_mhz(read_first(paths))

    # Haswell is an integrated GPU: it generally uses shared system RAM, not dedicated VRAM.
    # Report only counters actually exported by this kernel/driver.
    mem_paths = {
        "GTT total": dev / "mem_info_gtt_total",
        "GTT used": dev / "mem_info_gtt_used",
        "Dedicated VRAM total": dev / "mem_info_vram_total",
        "Dedicated VRAM used": dev / "mem_info_vram_used",
    }
    for label, path in mem_paths.items():
        raw = read_text(path)
        if raw is not None:
            try:
                result["memory"][label] = f"{int(raw):,} bytes ({int(raw) / (1024**2):.1f} MiB)"
            except ValueError:
                result["memory"][label] = raw
        else:
            result["memory"][label] = "Not exposed by this driver"

    rc6_paths = [dev / "power/rc6_residency_ms", dev / "gt/gt0/rc6_residency_ms"]
    rc6 = read_first(rc6_paths)
    result["power"]["RC6 idle residency"] = f"{int(rc6):,} ms" if rc6 and rc6.isdigit() else "Not exposed"
    result["card_sysfs"] = str(matched)
    return result


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("IDW — Intel GPU Monitor")
        self.geometry("760x720")
        self.minsize(650, 580)
        self.configure(padx=16, pady=14)
        self.idwctl = find_idwctl()
        self.after_id = None
        self.debugfs_data = None

        ttk.Label(self, text="IDW Intel GPU Monitor", font=("Sans", 20, "bold")).pack(anchor="w")
        ttk.Label(self, text="Live DRM/sysfs telemetry · on-demand Haswell i915 debugfs snapshot", foreground="#666").pack(anchor="w", pady=(2, 12))

        # Keep controls/status fixed while the metric panels scroll on smaller displays.
        content = ttk.Frame(self)
        content.pack(fill="both", expand=True)
        canvas = tk.Canvas(content, highlightthickness=0, borderwidth=0)
        scrollbar = ttk.Scrollbar(content, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)
        self.body = ttk.Frame(canvas)
        self.body_window = canvas.create_window((0, 0), window=self.body, anchor="nw")
        self.body.bind("<Configure>", lambda _event: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>", lambda event: canvas.itemconfigure(self.body_window, width=event.width))
        canvas.bind_all("<MouseWheel>", lambda event: canvas.yview_scroll(int(-event.delta / 120), "units"))

        self.identity = ttk.LabelFrame(self.body, text="Hardware identity", padding=10)
        self.identity.pack(fill="x", pady=5)
        self.identity_labels = self.add_metric_rows(self.identity, ["GPU", "Render node", "Kernel driver", "PCI address", "PCI IDs", "DRM card sysfs"])

        self.freq = ttk.LabelFrame(self.body, text="GPU frequency", padding=10)
        self.freq.pack(fill="x", pady=5)
        self.freq_labels = self.add_metric_rows(self.freq, ["Actual frequency", "Requested frequency", "Minimum frequency", "Maximum frequency", "Hardware maximum", "Hardware minimum"])

        self.memory = ttk.LabelFrame(self.body, text="Memory counters", padding=10)
        self.memory.pack(fill="x", pady=5)
        self.memory_labels = self.add_metric_rows(self.memory, ["GTT total", "GTT used", "Dedicated VRAM total", "Dedicated VRAM used"])

        self.power = ttk.LabelFrame(self.body, text="Power / idle", padding=10)
        self.power.pack(fill="x", pady=5)
        self.power_labels = self.add_metric_rows(self.power, ["RC6 idle residency"])

        self.hw = ttk.LabelFrame(self.body, text="Haswell hardware telemetry (i915 debugfs)", padding=10)
        self.hw.pack(fill="x", pady=5)
        self.hw_labels = self.add_metric_rows(self.hw, [
            "Current frequency", "Requested frequency", "Minimum frequency",
            "Nominal frequency", "Maximum frequency", "Boost frequency",
            "GPU execution units", "Slices / subslices", "GPU reset count",
            "GEM objects", "GEM object bytes", "Render engine", "Blitter engine",
            "Video engine", "Video Enhancement engine", "RPS enabled", "GPU busy",
        ])

        self.engine = ttk.LabelFrame(self.body, text="Engine activity", padding=10)
        self.engine.pack(fill="x", pady=5)
        ttk.Label(self.engine, text="Engine busy counters are provided through Intel's GPU PMU, not ordinary i915 sysfs files.").pack(anchor="w")
        ttk.Label(self.engine, text="Use intel_gpu_top for live Render / Blitter / Video utilization. Some systems require perf permissions.", foreground="#666", wraplength=690).pack(anchor="w", pady=(3, 7))

        controls = ttk.Frame(self)
        controls.pack(fill="x", pady=(12, 0))
        ttk.Button(controls, text="Refresh now", command=self.refresh).pack(side="left")
        ttk.Button(controls, text="Read privileged i915 telemetry", command=self.read_debugfs).pack(side="left", padx=7)
        ttk.Button(controls, text="Open intel_gpu_top", command=self.launch_gpu_top).pack(side="left", padx=7)
        ttk.Button(controls, text="Run GEM self-test", command=self.selftest).pack(side="left")
        ttk.Button(controls, text="Close", command=self.destroy).pack(side="right")
        self.status = ttk.Label(self, text="Starting monitor…", foreground="#555")
        self.status.pack(anchor="w", pady=(9, 0))
        self.refresh()

    @staticmethod
    def add_metric_rows(parent, names):
        labels = {}
        for row, name in enumerate(names):
            ttk.Label(parent, text=name, font=("Sans", 9, "bold")).grid(row=row, column=0, sticky="w", padx=(0, 20), pady=3)
            value = ttk.Label(parent, text="Reading…")
            value.grid(row=row, column=1, sticky="w", pady=3)
            labels[name] = value
        return labels

    def set_rows(self, labels, values):
        for key, widget in labels.items():
            widget.config(text=str(values.get(key, "Not available")))

    def refresh(self):
        if not self.idwctl:
            self.status.config(text="idwctl not found. Install it to ~/.local/bin or /usr/local/bin.")
            return
        try:
            proc = subprocess.run([self.idwctl, "info", "--json"], text=True, capture_output=True, check=True, timeout=5)
            adapters = json.loads(proc.stdout).get("adapters", [])
            if not adapters:
                self.status.config(text="No Intel DRM render adapter found.")
            else:
                a = adapters[0]
                metrics = sysfs_metrics(a)
                self.set_rows(self.identity_labels, {
                    "GPU": "Intel HD Graphics 4600 (Haswell GT2)" if a.get("device", "").lower() in ("0x0412", "0412") else f"PCI {a.get('vendor')}:{a.get('device')}",
                    "Render node": a.get("node", "Unknown"),
                    "Kernel driver": a.get("driver", "Unknown"),
                    "PCI address": a.get("pci_path", "Unknown"),
                    "PCI IDs": f"{a.get('vendor', '?')}:{a.get('device', '?')}  rev {a.get('revision', '?')}",
                    "DRM card sysfs": metrics.get("card_sysfs", "Not matched"),
                })
                self.set_rows(self.freq_labels, metrics["frequency"])
                self.set_rows(self.memory_labels, metrics["memory"])
                self.set_rows(self.power_labels, metrics["power"])
                if self.debugfs_data:
                    self.set_rows(self.hw_labels, self.debugfs_data)
                self.status.config(text=f"Monitoring {len(adapters)} adapter(s) · refresh every {REFRESH_MS/1000:.1f}s · values are kernel-reported")
        except Exception as exc:
            self.status.config(text=f"Monitor read failed: {exc}")
        self.after_id = self.after(REFRESH_MS, self.refresh)

    def read_debugfs(self):
        """Read a fixed, read-only snapshot through a root-owned helper; no continuous root polling."""
        helper = Path(PRIVILEGED_HELPER)
        pkexec = shutil.which("pkexec")
        if not helper.is_file():
            messagebox.showinfo(
                "IDW telemetry helper not installed",
                "Install the read-only helper from the project directory:\n\n"
                "sudo make install-system-helper\n\n"
                "Then click this button again."
            )
            return
        if not pkexec:
            messagebox.showerror("IDW", "pkexec is not installed. Install polkit.")
            return
        try:
            proc = subprocess.run([pkexec, str(helper)], text=True, capture_output=True, timeout=20)
            if proc.returncode != 0:
                detail = (proc.stderr or proc.stdout or "Authentication cancelled or helper failed.").strip()
                messagebox.showwarning("IDW telemetry", detail)
                return
            data = json.loads(proc.stdout)
            self.debugfs_data = data.get("metrics", {})
            self.set_rows(self.hw_labels, self.debugfs_data)
            self.status.config(text="Privileged i915 debugfs snapshot loaded · click again to take a new snapshot")
        except Exception as exc:
            messagebox.showerror("IDW telemetry", str(exc))

    def launch_gpu_top(self):
        executable = shutil.which("intel_gpu_top")
        if not executable:
            messagebox.showinfo("IDW", "intel_gpu_top is not installed. Install it with:\n\nsudo pacman -S intel-gpu-tools")
            return
        terminals = [
            ("konsole", ["konsole", "-e", executable]),
            ("xterm", ["xterm", "-e", executable]),
            ("xfce4-terminal", ["xfce4-terminal", "--", executable]),
        ]
        for terminal, command in terminals:
            if shutil.which(terminal):
                try:
                    subprocess.Popen(command)
                    return
                except OSError as exc:
                    messagebox.showerror("IDW", f"Could not launch {terminal}:\n{exc}")
                    return
        messagebox.showinfo("IDW", "Install a terminal emulator (Konsole is recommended) to display intel_gpu_top.")

    def selftest(self):
        if not self.idwctl:
            messagebox.showerror("IDW", "idwctl was not found.")
            return
        try:
            proc = subprocess.run([self.idwctl, "selftest"], text=True, capture_output=True, timeout=15)
            output = (proc.stdout + proc.stderr).strip()
            messagebox.showinfo("IDW GEM self-test", output or f"Exit code: {proc.returncode}")
            self.status.config(text=f"GEM self-test exit code: {proc.returncode}")
        except Exception as exc:
            messagebox.showerror("IDW", str(exc))

    def destroy(self):
        if self.after_id:
            try:
                self.after_cancel(self.after_id)
            except tk.TclError:
                pass
        super().destroy()


if __name__ == "__main__":
    App().mainloop()
